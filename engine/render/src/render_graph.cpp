#include "fledge/render/render_graph.h"

#include <algorithm>
#include <format>
#include <queue>
#include <set>

#include "fledge/core/log.h"

namespace fledge {

// ---------------------------------------------------------------- builder --
RGResource RGBuilder::create(const std::string& name, const rhi::TextureDesc& desc) {
    m_graph.m_resources.push_back({name, desc});
    RGResource r = static_cast<RGResource>(m_graph.m_resources.size() - 1);
    m_graph.m_passes[m_pass].creates.push_back(r);
    return r;
}
void RGBuilder::read(RGResource r) {
    if (r != kRGInvalid) m_graph.m_passes[m_pass].reads.push_back(r);
}
void RGBuilder::write(RGResource r, int mip, int layer) {
    if (r != kRGInvalid) m_graph.m_passes[m_pass].colors.push_back({r, mip, layer});
}
void RGBuilder::writeDepth(RGResource r, int mip, int layer) {
    if (r != kRGInvalid) m_graph.m_passes[m_pass].depth = RenderGraph::Write{r, mip, layer};
}
void RGBuilder::clear(const rhi::ClearDesc& c) { m_graph.m_passes[m_pass].clear = c; }
void RGBuilder::viewport(int w, int h) {
    m_graph.m_passes[m_pass].vpW = w;
    m_graph.m_passes[m_pass].vpH = h;
}
void RGBuilder::sideEffect() { m_graph.m_passes[m_pass].sideEffect = true; }

rhi::TextureHandle RGContext::texture(RGResource r) const { return graph.texture(r); }

// ------------------------------------------------------------------ pool ---
TransientTexturePool::~TransientTexturePool() {
    for (auto& [k, fb] : m_fbCache) m_device.destroyFramebuffer(fb);
    for (auto& e : m_entries) m_device.destroyTexture(e.tex);
}

rhi::TextureHandle TransientTexturePool::acquire(const rhi::TextureDesc& desc) {
    for (auto& e : m_entries)
        if (!e.inUse && e.desc == desc) {
            e.inUse = true;
            e.lastFrame = m_frame;
            return e.tex;
        }
    Entry e{desc, m_device.createTexture(desc), true, m_frame};
    m_entries.push_back(e);
    return e.tex;
}

void TransientTexturePool::release(rhi::TextureHandle tex) {
    for (auto& e : m_entries)
        if (e.tex == tex) e.inUse = false;
}

rhi::FramebufferHandle TransientTexturePool::framebuffer(const rhi::FramebufferDesc& d) {
    std::vector<uint32_t> key;
    for (int i = 0; i < d.colorCount; ++i) {
        auto& a = d.colors[static_cast<size_t>(i)];
        key.insert(key.end(), {a.texture.id, static_cast<uint32_t>(a.mip), static_cast<uint32_t>(a.layer)});
    }
    key.insert(key.end(), {0xFFFFFFFFu, d.depth.texture.id, static_cast<uint32_t>(d.depth.mip), static_cast<uint32_t>(d.depth.layer)});
    if (auto it = m_fbCache.find(key); it != m_fbCache.end()) return it->second;
    auto fb = m_device.createFramebuffer(d);
    m_fbCache[key] = fb;
    return fb;
}

void TransientTexturePool::endFrame(int maxIdle) {
    ++m_frame;
    std::set<uint32_t> dead;
    for (auto& e : m_entries) {
        e.inUse = false;
        if (m_frame - e.lastFrame > static_cast<uint64_t>(maxIdle)) dead.insert(e.tex.id);
    }
    if (dead.empty()) return;
    for (auto it = m_fbCache.begin(); it != m_fbCache.end();) {
        bool uses = false;
        for (size_t i = 0; i < it->first.size(); ++i)
            if (dead.contains(it->first[i])) uses = true;
        if (uses) {
            m_device.destroyFramebuffer(it->second);
            it = m_fbCache.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(m_entries, [&](const Entry& e) {
        if (!dead.contains(e.tex.id)) return false;
        m_device.destroyTexture(e.tex);
        return true;
    });
}

// ----------------------------------------------------------------- graph ---
RGResource RenderGraph::importTexture(const std::string& name, rhi::TextureHandle tex, const rhi::TextureDesc& desc) {
    Resource r{name, desc};
    r.imported = true;
    r.texture = tex;
    m_resources.push_back(r);
    return static_cast<RGResource>(m_resources.size() - 1);
}

RGResource RenderGraph::importFramebuffer(const std::string& name, rhi::FramebufferHandle fb, int w, int h) {
    Resource r;
    r.name = name;
    r.imported = true;
    r.isFramebuffer = true;
    r.framebuffer = fb;
    r.fbWidth = w;
    r.fbHeight = h;
    m_resources.push_back(r);
    return static_cast<RGResource>(m_resources.size() - 1);
}

void RenderGraph::addPass(const std::string& name, const std::function<void(RGBuilder&)>& setup, ExecFn execute) {
    m_passes.push_back({});
    m_passes.back().name = name;
    m_passes.back().exec = std::move(execute);
    RGBuilder b(*this, m_passes.size() - 1);
    setup(b);
    m_compiled = false;
}

void RenderGraph::markOutput(RGResource r) {
    if (r < m_resources.size()) m_resources[r].output = true;
}

void RenderGraph::reset() {
    m_resources.clear();
    m_passes.clear();
    m_order.clear();
    m_orderNames.clear();
    m_compiled = false;
}

std::vector<std::string> RenderGraph::culledPasses() const {
    std::vector<std::string> out;
    for (auto& p : m_passes)
        if (p.culled) out.push_back(p.name);
    return out;
}

Status RenderGraph::compile() {
    const size_t n = m_passes.size();
    // writers per resource (declaration order)
    std::vector<std::vector<size_t>> writers(m_resources.size());
    for (size_t i = 0; i < n; ++i) {
        for (auto& w : m_passes[i].colors) writers[w.res].push_back(i);
        if (m_passes[i].depth) writers[m_passes[i].depth->res].push_back(i);
    }
    // edges: producer -> consumer
    std::vector<std::set<size_t>> deps(n);  // deps[i] = passes that must run before i
    for (size_t i = 0; i < n; ++i) {
        // Read-after-write: depend on the writers declared before the reader (versioned semantics,
        // so "read X, later overwrite X" chains like bloom up-sampling stay acyclic). If none were
        // declared earlier, the reader was declared out of order: depend on all writers.
        for (RGResource r : m_passes[i].reads) {
            bool anyEarlier = false;
            for (size_t w : writers[r])
                if (w < i) {
                    deps[i].insert(w);
                    anyEarlier = true;
                }
            if (!anyEarlier)
                for (size_t w : writers[r])
                    if (w != i) deps[i].insert(w);
        }
        auto writeDeps = [&](RGResource r) {  // earlier writers of the same resource (load / overwrite order)
            for (size_t w : writers[r])
                if (w < i) deps[i].insert(w);
        };
        for (auto& w : m_passes[i].colors) writeDeps(w.res);
        if (m_passes[i].depth) writeDeps(m_passes[i].depth->res);
    }
    // culling: keep passes that write outputs or have side effects, plus everything they depend on
    std::vector<bool> needed(n, false);
    std::vector<size_t> stack;
    for (size_t i = 0; i < n; ++i) {
        bool out = m_passes[i].sideEffect;
        for (auto& w : m_passes[i].colors) out |= m_resources[w.res].output;
        if (m_passes[i].depth) out |= m_resources[m_passes[i].depth->res].output;
        if (out) stack.push_back(i);
    }
    while (!stack.empty()) {
        size_t p = stack.back();
        stack.pop_back();
        if (needed[p]) continue;
        needed[p] = true;
        for (size_t d : deps[p]) stack.push_back(d);
    }
    // Kahn's algorithm, stable by declaration index
    std::vector<int> indeg(n, 0);
    std::vector<std::vector<size_t>> users(n);
    for (size_t i = 0; i < n; ++i) {
        m_passes[i].culled = !needed[i];
        if (!needed[i]) continue;
        for (size_t d : deps[i])
            if (needed[d]) {
                ++indeg[i];
                users[d].push_back(i);
            }
    }
    std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
    for (size_t i = 0; i < n; ++i)
        if (needed[i] && indeg[i] == 0) ready.push(i);
    m_order.clear();
    m_orderNames.clear();
    while (!ready.empty()) {
        size_t p = ready.top();
        ready.pop();
        m_order.push_back(p);
        m_orderNames.push_back(m_passes[p].name);
        for (size_t u : users[p])
            if (--indeg[u] == 0) ready.push(u);
    }
    size_t neededCount = static_cast<size_t>(std::count(needed.begin(), needed.end(), true));
    if (m_order.size() != neededCount) {
        m_compiled = false;
        return Status::failure("render graph has a dependency cycle");
    }
    m_compiled = true;
    return {};
}

void RenderGraph::execute(rhi::RenderDevice& device, TransientTexturePool& pool) {
    if (!m_compiled) {
        if (auto st = compile(); !st) {
            FLEDGE_LOG_ERROR("render", "{}", st.error);
            return;
        }
    }
    // Last use of each transient resource (for in-frame aliasing via the pool).
    std::vector<int> lastUse(m_resources.size(), -1);
    for (size_t k = 0; k < m_order.size(); ++k) {
        const Pass& p = m_passes[m_order[k]];
        for (RGResource r : p.reads) lastUse[r] = static_cast<int>(k);
        for (auto& w : p.colors) lastUse[w.res] = static_cast<int>(k);
        if (p.depth) lastUse[p.depth->res] = static_cast<int>(k);
    }
    for (size_t k = 0; k < m_order.size(); ++k) {
        Pass& p = m_passes[m_order[k]];
        auto ensure = [&](RGResource r) {
            Resource& res = m_resources[r];
            if (!res.imported && !res.texture) res.texture = pool.acquire(res.desc);
        };
        for (RGResource r : p.reads) ensure(r);
        for (auto& w : p.colors) ensure(w.res);
        if (p.depth) ensure(p.depth->res);

        rhi::FramebufferHandle fb;
        int w = p.vpW, h = p.vpH;
        const Resource* first = !p.colors.empty() ? &m_resources[p.colors[0].res] : p.depth ? &m_resources[p.depth->res] : nullptr;
        if (first && first->isFramebuffer) {
            fb = first->framebuffer;
            if (!w) w = first->fbWidth;
            if (!h) h = first->fbHeight;
        } else if (first) {
            rhi::FramebufferDesc fd;
            for (auto& c : p.colors) fd.colors[static_cast<size_t>(fd.colorCount++)] = {m_resources[c.res].texture, c.mip, c.layer};
            if (p.depth) fd.depth = {m_resources[p.depth->res].texture, p.depth->mip, p.depth->layer};
            fb = pool.framebuffer(fd);
            int mip = !p.colors.empty() ? p.colors[0].mip : p.depth->mip;
            if (!w) w = std::max(1, first->desc.width >> mip);
            if (!h) h = std::max(1, first->desc.height >> mip);
        }
        RGContext ctx{device, fb, w, h, *this};
        if (first) device.beginPass(fb, 0, 0, w, h, p.clear);
        if (p.exec) p.exec(ctx);
        if (first) device.endPass();
        // release transients whose last use was this pass
        for (size_t r = 0; r < m_resources.size(); ++r)
            if (lastUse[r] == static_cast<int>(k) && !m_resources[r].imported && m_resources[r].texture && !m_resources[r].output)
                pool.release(m_resources[r].texture);
    }
}

} // namespace fledge
