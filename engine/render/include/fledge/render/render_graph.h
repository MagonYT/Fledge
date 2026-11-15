#pragma once
// Lightweight frame graph.
//
//   RenderGraph g;
//   auto shadow = ...; auto hdr = ...;
//   g.addPass("main", [&](RGBuilder& b) { b.read(shadow); hdr = b.create("hdr", desc); b.write(hdr); b.writeDepth(depth); },
//             [&](RGContext& ctx) { ... ctx.device.draw(...) ... });
//   g.markOutput(final);
//   g.compile();               // dependency DAG -> stable topological order, cull passes nobody needs
//   g.execute(device, pool);   // transient textures from the pool (reused across passes/frames),
//                              // framebuffers built from each pass's writes, begin/endPass handled
//
// Dependencies: a pass that reads R depends on R's writers (writers of the same resource are
// ordered by declaration). Passes may be declared in any order; cycles are reported by compile().
// Passes that contribute to no output (and are not marked sideEffect) are culled.
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "fledge/core/utility.h"
#include "fledge/render/rhi.h"

namespace fledge {

using RGResource = uint32_t;
constexpr RGResource kRGInvalid = ~0u;

class RenderGraph;

class RGBuilder {
public:
    RGResource create(const std::string& name, const rhi::TextureDesc& desc);  // transient texture
    void read(RGResource r);
    // Color attachment (in call order). mip/layer select a subresource (cube face / array layer).
    void write(RGResource r, int mip = 0, int layer = 0);
    void writeDepth(RGResource r, int mip = 0, int layer = 0);
    void clear(const rhi::ClearDesc& c);  // default: clear color+depth
    void viewport(int width, int height);  // default: size of the first attachment (at its mip)
    void sideEffect();                     // never culled (e.g. writes to an imported framebuffer)

private:
    friend class RenderGraph;
    RGBuilder(RenderGraph& g, size_t pass) : m_graph(g), m_pass(pass) {}
    RenderGraph& m_graph;
    size_t m_pass;
};

struct RGContext {
    rhi::RenderDevice& device;
    rhi::FramebufferHandle framebuffer;
    int width = 0, height = 0;
    const RenderGraph& graph;
    rhi::TextureHandle texture(RGResource r) const;
};

class TransientTexturePool {
public:
    explicit TransientTexturePool(rhi::RenderDevice& device) : m_device(device) {}
    ~TransientTexturePool();
    rhi::TextureHandle acquire(const rhi::TextureDesc& desc);
    void release(rhi::TextureHandle tex);
    rhi::FramebufferHandle framebuffer(const rhi::FramebufferDesc& desc);
    void endFrame(int maxIdleFrames = 30);  // destroys textures unused for a while
    size_t textureCount() const { return m_entries.size(); }
    rhi::RenderDevice& device() { return m_device; }

private:
    struct Entry {
        rhi::TextureDesc desc;
        rhi::TextureHandle tex;
        bool inUse = false;
        uint64_t lastFrame = 0;
    };
    rhi::RenderDevice& m_device;
    std::vector<Entry> m_entries;
    std::map<std::vector<uint32_t>, rhi::FramebufferHandle> m_fbCache;
    uint64_t m_frame = 0;
};

class RenderGraph {
public:
    using ExecFn = std::function<void(RGContext&)>;

    RGResource importTexture(const std::string& name, rhi::TextureHandle tex, const rhi::TextureDesc& desc);
    // An external render destination (backbuffer = {}, or an editor render target's framebuffer).
    RGResource importFramebuffer(const std::string& name, rhi::FramebufferHandle fb, int width, int height);
    void addPass(const std::string& name, const std::function<void(RGBuilder&)>& setup, ExecFn execute);
    void markOutput(RGResource r);

    Status compile();
    void execute(rhi::RenderDevice& device, TransientTexturePool& pool);
    void reset();

    const std::vector<std::string>& executionOrder() const { return m_orderNames; }  // after compile()
    std::vector<std::string> culledPasses() const;
    const rhi::TextureDesc& desc(RGResource r) const { return m_resources.at(r).desc; }
    rhi::TextureHandle texture(RGResource r) const { return r < m_resources.size() ? m_resources[r].texture : rhi::TextureHandle{}; }
    size_t passCount() const { return m_passes.size(); }

private:
    friend class RGBuilder;
    struct Resource {
        std::string name;
        rhi::TextureDesc desc;
        bool imported = false;
        bool isFramebuffer = false;
        rhi::FramebufferHandle framebuffer;
        int fbWidth = 0, fbHeight = 0;
        rhi::TextureHandle texture;
        bool output = false;
    };
    struct Write {
        RGResource res;
        int mip, layer;
    };
    struct Pass {
        std::string name;
        std::vector<RGResource> reads;
        std::vector<Write> colors;
        std::optional<Write> depth;
        rhi::ClearDesc clear;
        int vpW = 0, vpH = 0;
        bool sideEffect = false;
        ExecFn exec;
        bool culled = false;
        std::vector<RGResource> creates;
    };
    std::vector<Resource> m_resources;
    std::vector<Pass> m_passes;
    std::vector<size_t> m_order;
    std::vector<std::string> m_orderNames;
    bool m_compiled = false;
};

} // namespace fledge
