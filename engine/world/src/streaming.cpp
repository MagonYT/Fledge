#include "fledge/world/streaming.h"

#include <algorithm>
#include <cmath>

#include "fledge/core/log.h"

namespace fledge::world {

const char* toString(ChunkState s) {
    switch (s) {
        case ChunkState::Loading: return "loading";
        case ChunkState::Loaded: return "loaded";
        default: return "unloaded";
    }
}

// ============================================================================== scheduler ===
void StreamingScheduler::setState(ChunkCoord c, ChunkState s) { m_chunks[c].state = s; }

ChunkState StreamingScheduler::state(ChunkCoord c) const {
    auto it = m_chunks.find(c);
    return it == m_chunks.end() ? ChunkState::Unloaded : it->second.state;
}

size_t StreamingScheduler::residentBytes() const {
    size_t b = 0;
    for (auto& [c, ch] : m_chunks)
        if (ch.state != ChunkState::Unloaded) b += ch.bytes;
    return b;
}

int StreamingScheduler::inFlight() const {
    int n = 0;
    for (auto& [c, ch] : m_chunks) n += ch.state == ChunkState::Loading ? 1 : 0;
    return n;
}

StreamingScheduler::Plan StreamingScheduler::update(double px, double pz, double vx, double vz, double now) {
    Plan plan;
    double speed = std::sqrt(vx * vx + vz * vz);
    double lead = std::min(speed * 2.0, m_s.cellSize);  // look ~2 s ahead, at most one cell
    std::vector<std::pair<double, ChunkCoord>> wanted;
    for (auto& [c, ch] : m_chunks) {
        double d = distanceToChunk(c, m_s.cellSize, px, pz);
        double cx = (c.x + 0.5) * m_s.cellSize - px, cz = (c.z + 0.5) * m_s.cellSize - pz;
        double len = std::sqrt(cx * cx + cz * cz);
        double ahead = (speed > 0.1 && len > 1e-6) ? (cx * vx + cz * vz) / (len * speed) : 0.0;  // -1..1
        ch.priority = d - ahead * lead;
        if (d <= m_s.loadRadius) {
            ch.lastNeeded = now;
            if (ch.state == ChunkState::Unloaded) wanted.push_back({ch.priority, c});
        } else if (d > m_s.unloadRadius && ch.state == ChunkState::Loaded) {
            plan.unload.push_back(c);
        }
    }
    std::sort(wanted.begin(), wanted.end());
    // Memory budget: evict loaded chunks that are not needed right now (oldest need first).
    size_t resident = residentBytes();
    for (auto& c : plan.unload) resident -= m_chunks[c].bytes;
    size_t incoming = 0;
    for (auto& [p, c] : wanted) incoming += m_chunks[c].bytes;
    if (resident + incoming > m_s.memoryBudget) {
        std::vector<std::pair<double, ChunkCoord>> evictable;
        for (auto& [c, ch] : m_chunks)
            if (ch.state == ChunkState::Loaded && ch.lastNeeded < now && std::find(plan.unload.begin(), plan.unload.end(), c) == plan.unload.end())
                evictable.push_back({ch.lastNeeded, c});
        std::sort(evictable.begin(), evictable.end());
        for (auto& [t, c] : evictable) {
            if (resident + incoming <= m_s.memoryBudget) break;
            plan.unload.push_back(c);
            resident -= m_chunks[c].bytes;
        }
    }
    int slots = m_s.maxInFlight - inFlight();
    for (auto& [p, c] : wanted) {
        if (slots <= 0) break;
        if (resident + m_chunks[c].bytes > m_s.memoryBudget) continue;  // over budget even after eviction: skip
        plan.load.push_back(c);
        resident += m_chunks[c].bytes;
        --slots;
    }
    return plan;
}

// ============================================================================== sources ===
MemoryChunkSource::MemoryChunkSource(const Partition& p) {
    for (auto& [c, ch] : p.chunks) m_text[c] = ch.text();
}
std::vector<std::pair<ChunkCoord, size_t>> MemoryChunkSource::list() const {
    std::vector<std::pair<ChunkCoord, size_t>> out;
    for (auto& [c, t] : m_text) out.push_back({c, t.size()});
    return out;
}
std::optional<std::string> MemoryChunkSource::read(ChunkCoord c) const {
    auto it = m_text.find(c);
    if (it == m_text.end()) return std::nullopt;
    return it->second;
}

VfsChunkSource::VfsChunkSource(Vfs& vfs, std::string worldDir, const Json& index) : m_vfs(vfs), m_dir(std::move(worldDir)) {
    for (auto& c : index.value("chunks", Json::array()))
        m_files[{c.value("x", 0), c.value("z", 0)}] = {c.value("name", ""), c.value("bytes", size_t{0})};
}
std::vector<std::pair<ChunkCoord, size_t>> VfsChunkSource::list() const {
    std::vector<std::pair<ChunkCoord, size_t>> out;
    for (auto& [c, f] : m_files) out.push_back({c, f.second});
    return out;
}
std::optional<std::string> VfsChunkSource::read(ChunkCoord c) const {
    auto it = m_files.find(c);
    if (it == m_files.end()) return std::nullopt;
    std::lock_guard l(m_mutex);
    return m_vfs.readText(m_dir + "/" + it->second.first);
}

// ============================================================================== streaming world ===
StreamingWorld::StreamingWorld(Scene& scene, PartitionSettings settings, std::shared_ptr<ChunkSource> source, JobSystem* jobs)
    : m_scene(scene), m_settings(std::move(settings)), m_source(std::move(source)), m_jobs(jobs), m_alive(std::make_shared<std::atomic<bool>>(true)) {
    StreamingScheduler::Settings s;
    s.cellSize = m_settings.cellSize;
    s.loadRadius = m_settings.loadRadius;
    s.unloadRadius = m_settings.unloadRadius;
    s.memoryBudget = m_settings.memoryBudget;
    m_sched.setSettings(s);
    for (auto& [c, bytes] : m_source->list()) m_sched.addChunk(c, bytes);
    m_stats.known = static_cast<int>(m_source->list().size());
}

StreamingWorld::~StreamingWorld() {
    *m_alive = false;
    for (auto& f : m_inflight)
        if (f.done.valid()) f.done.wait();
}

std::unique_ptr<StreamingWorld> StreamingWorld::fromLiveScene(Scene& scene, JobSystem* jobs) {
    const WorldPartitionComponent* wp = findPartitionSettings(scene);
    if (!wp) return nullptr;
    PartitionSettings ps = PartitionSettings::from(*wp);
    Partition part = buildPartition(scene, ps);
    auto src = std::make_shared<MemoryChunkSource>(part);
    for (auto& [c, ch] : part.chunks)
        for (EntityId r : ch.roots) scene.destroyEntity(r);  // streamed out until needed
    auto w = std::make_unique<StreamingWorld>(scene, ps, src, jobs);
    FLEDGE_LOG_INFO("world", "Streaming: {} chunks ({:.1f} KB), cell {:.0f} m, load {:.0f} m / unload {:.0f} m", part.chunks.size(),
                    part.totalBytes / 1024.0, ps.cellSize, ps.loadRadius, ps.unloadRadius);
    return w;
}

void StreamingWorld::startLoad(ChunkCoord c) {
    m_sched.setState(c, ChunkState::Loading);
    auto out = std::make_shared<Json>();
    auto src = m_source;
    auto job = [src, c, out] {
        if (auto t = src->read(c)) *out = parseChunk(*t);
    };
    if (m_jobs) {
        m_inflight.push_back({c, out, m_jobs->submit(job)});
    } else {
        job();
        m_ready.push_back({c, std::move(*out)});
    }
}

void StreamingWorld::unload(ChunkCoord c) {
    auto it = m_loaded.find(c);
    if (it != m_loaded.end()) {
        for (EntityId e : it->second)
            if (m_scene.isValid(e)) m_scene.destroyEntity(e);
        m_loaded.erase(it);
    }
    // a chunk still being applied is dropped too
    for (auto p = m_ready.begin(); p != m_ready.end();)
        if (p->coord == c) {
            for (EntityId e : p->roots)
                if (m_scene.isValid(e)) m_scene.destroyEntity(e);
            p = m_ready.erase(p);
        } else {
            ++p;
        }
    m_sched.setState(c, ChunkState::Unloaded);
    ++m_stats.unloadsTotal;
}

bool StreamingWorld::applySome(double budgetMs) {
    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    bool any = false;
    while (!m_ready.empty() && (budgetMs <= 0 || elapsed() < budgetMs)) {
        Pending& p = m_ready.front();
        const Json& ents = p.data.is_object() ? p.data["entities"] : Json::array();
        // entity by entity so one huge chunk cannot blow the frame budget
        while (p.cursor < ents.size() && (budgetMs <= 0 || elapsed() < budgetMs)) {
            const Json& ent = ents[p.cursor++];
            EntityId id = ent.value("id", kNullEntity);
            std::string layer = ent.value("layer", "");
            const Json& st = ent["state"];
            EntityId parent = st.value("parent", kNullEntity);
            if ((!layer.empty() && !m_settings.activeLayers.count(layer)) || p.skipped.count(parent)) {
                p.skipped.insert(id);
                continue;
            }
            if (!m_scene.applyEntityState(id, st)) continue;
            if (parent == kNullEntity) {
                if (auto* t = m_scene.get<TransformComponent>(id)) t->position = (DVec3::from(t->position) - m_origin).toVec3();
                p.roots.push_back(id);
            }
            any = true;
        }
        if (p.cursor >= ents.size()) {
            m_loaded[p.coord] = std::move(p.roots);
            m_sched.setState(p.coord, ChunkState::Loaded);
            ++m_stats.loadsTotal;
            m_ready.pop_front();
        }
    }
    double ms = elapsed();
    m_stats.applyMsLast = ms;
    m_stats.applyMsMax = std::max(m_stats.applyMsMax, ms);
    return any;
}

void StreamingWorld::rebase(Vec3 delta) {
    for (EntityId r : m_scene.roots())
        if (auto* t = m_scene.get<TransformComponent>(r)) t->position -= delta;
    m_origin = m_origin + DVec3::from(delta);
    ++m_stats.rebases;
    FLEDGE_LOG_INFO("world", "Floating origin moved to ({:.0f}, {:.0f}, {:.0f})", m_origin.x, m_origin.y, m_origin.z);
}

Vec3 StreamingWorld::update(Vec3 viewerLocal, Vec3 vel, double now) {
    Vec3 delta{0, 0, 0};
    double horiz = std::sqrt(static_cast<double>(viewerLocal.x) * viewerLocal.x + static_cast<double>(viewerLocal.z) * viewerLocal.z);
    if (horiz > m_settings.rebaseDistance) {  // snap to whole cells so chunk-aligned data stays exactly representable
        double c = m_settings.cellSize;
        delta = {static_cast<float>(std::round(viewerLocal.x / c) * c), 0.0f, static_cast<float>(std::round(viewerLocal.z / c) * c)};
        rebase(delta);
        viewerLocal -= delta;
    }
    DVec3 w = toWorld(viewerLocal);
    // collect finished reads
    for (auto it = m_inflight.begin(); it != m_inflight.end();) {
        if (it->done.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            if (m_sched.state(it->coord) == ChunkState::Loading) m_ready.push_back({it->coord, std::move(*it->data)});
            it = m_inflight.erase(it);
        } else {
            ++it;
        }
    }
    auto plan = m_sched.update(w.x, w.z, vel.x, vel.z, now);
    for (auto& c : plan.unload) {
        bool evict = distanceToChunk(c, m_settings.cellSize, w.x, w.z) <= m_settings.unloadRadius;
        unload(c);
        if (evict) ++m_stats.evictions;
    }
    for (auto& c : plan.load) startLoad(c);
    applySome(m_settings.frameBudgetMs);
    m_stats.loaded = static_cast<int>(m_loaded.size());
    m_stats.loading = static_cast<int>(m_inflight.size() + m_ready.size());
    m_stats.residentBytes = m_sched.residentBytes();
    m_stats.entities = static_cast<int>(m_scene.entities().size());
    m_stats.origin = m_origin;
    return delta;
}

void StreamingWorld::loadAround(Vec3 viewerLocal) {
    DVec3 w = toWorld(viewerLocal);
    for (auto& [c, ch] : m_sched.chunks())
        if (ch.state == ChunkState::Unloaded && distanceToChunk(c, m_settings.cellSize, w.x, w.z) <= m_settings.loadRadius) {
            auto t = m_source->read(c);
            m_sched.setState(c, ChunkState::Loading);
            m_ready.push_back({c, t ? parseChunk(*t) : Json()});
        }
    applySome(0);
    m_stats.loaded = static_cast<int>(m_loaded.size());
}

void StreamingWorld::loadAll() {
    for (auto& f : m_inflight)
        if (f.done.valid()) f.done.wait();
    m_inflight.clear();
    for (auto& [c, ch] : m_sched.chunks())
        if (ch.state != ChunkState::Loaded) {
            auto t = m_source->read(c);
            m_sched.setState(c, ChunkState::Loading);
            m_ready.push_back({c, t ? parseChunk(*t) : Json()});
        }
    applySome(0);
}

void StreamingWorld::setActiveLayers(const std::set<std::string>& layers) {
    if (layers == m_settings.activeLayers) return;
    m_settings.activeLayers = layers;
    std::vector<ChunkCoord> reload;
    for (auto& [c, ents] : m_loaded) reload.push_back(c);
    for (auto& c : reload) {
        unload(c);
        startLoad(c);
    }
    for (auto& f : m_inflight)
        if (f.done.valid()) f.done.wait();
}

} // namespace fledge::world
