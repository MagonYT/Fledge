#pragma once
// Streaming: scheduler (pure, unit-tested) + StreamingWorld (scene integration, async IO, frame budget,
// memory budget, floating origin). See docs/WORLD_STREAMING.md.
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fledge/core/jobs.h"
#include "fledge/core/vfs.h"
#include "fledge/world/partition.h"

namespace fledge::world {

enum class ChunkState { Unloaded, Loading, Loaded };
const char* toString(ChunkState s);

class StreamingScheduler {
public:
    struct Settings {
        double cellSize = 128, loadRadius = 384, unloadRadius = 512;
        size_t memoryBudget = 256ull << 20;
        int maxInFlight = 4;
    };
    struct Chunk {
        ChunkState state = ChunkState::Unloaded;
        size_t bytes = 0;
        double lastNeeded = -1e30;
        double priority = 0;
    };
    struct Plan {
        std::vector<ChunkCoord> load;    // highest priority first
        std::vector<ChunkCoord> unload;  // out of range, or evicted for the memory budget
    };

    StreamingScheduler() = default;
    explicit StreamingScheduler(Settings s) : m_s(s) {}
    void setSettings(const Settings& s) { m_s = s; }
    const Settings& settings() const { return m_s; }
    void addChunk(ChunkCoord c, size_t bytes) { m_chunks[c].bytes = bytes; }
    // Viewer position (absolute world x/z) and velocity (m/s, used to favour the direction of travel).
    Plan update(double px, double pz, double vx, double vz, double now);
    void setState(ChunkCoord c, ChunkState s);
    ChunkState state(ChunkCoord c) const;
    size_t residentBytes() const;  // loaded + loading
    const std::map<ChunkCoord, Chunk>& chunks() const { return m_chunks; }
    int inFlight() const;

private:
    Settings m_s;
    std::map<ChunkCoord, Chunk> m_chunks;
};

// Where chunk data comes from: memory (play-in-editor) or the VFS (cooked pak / folder).
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    virtual std::vector<std::pair<ChunkCoord, size_t>> list() const = 0;
    virtual std::optional<std::string> read(ChunkCoord c) const = 0;  // thread-safe
};
class MemoryChunkSource : public ChunkSource {
public:
    explicit MemoryChunkSource(const Partition& p);
    std::vector<std::pair<ChunkCoord, size_t>> list() const override;
    std::optional<std::string> read(ChunkCoord c) const override;

private:
    std::map<ChunkCoord, std::string> m_text;
};
class VfsChunkSource : public ChunkSource {
public:
    VfsChunkSource(Vfs& vfs, std::string worldDir, const Json& index);
    std::vector<std::pair<ChunkCoord, size_t>> list() const override;
    std::optional<std::string> read(ChunkCoord c) const override;

private:
    Vfs& m_vfs;
    std::string m_dir;
    std::map<ChunkCoord, std::pair<std::string, size_t>> m_files;
    mutable std::mutex m_mutex;  // Vfs providers are not documented as thread-safe
};

class StreamingWorld {
public:
    struct Stats {
        int known = 0, loaded = 0, loading = 0, entities = 0;
        size_t residentBytes = 0;
        double applyMsLast = 0, applyMsMax = 0;  // main-thread chunk apply time
        int loadsTotal = 0, unloadsTotal = 0, evictions = 0, rebases = 0;
        DVec3 origin;
    };

    // jobs == nullptr: chunks are read synchronously (tests, tools).
    StreamingWorld(Scene& scene, PartitionSettings settings, std::shared_ptr<ChunkSource> source, JobSystem* jobs);
    ~StreamingWorld();

    // Play-in-editor: partitions the live scene, streams every chunk out and builds an in-memory source.
    static std::unique_ptr<StreamingWorld> fromLiveScene(Scene& scene, JobSystem* jobs);

    // Per frame. viewer = local (floating-origin) position. Returns the rebase delta applied this frame
    // (zero if none) so callers can move their own cameras by it.
    Vec3 update(Vec3 viewerLocal, Vec3 viewerVelocity, double nowSeconds);
    // Loads everything needed around the viewer now (no budget): game start, teleports.
    void loadAround(Vec3 viewerLocal);
    void setActiveLayers(const std::set<std::string>& layers);  // reloads affected chunks
    // Restores every streamed-out chunk (Stop in the editor does a full scene restore instead).
    void loadAll();

    ChunkState state(ChunkCoord c) const { return m_sched.state(c); }
    const StreamingScheduler& scheduler() const { return m_sched; }
    const PartitionSettings& settings() const { return m_settings; }
    const Stats& stats() const { return m_stats; }
    DVec3 origin() const { return m_origin; }
    DVec3 toWorld(Vec3 local) const { return DVec3::from(local) + m_origin; }

private:
    struct Pending {
        ChunkCoord coord;
        Json data;
        size_t cursor = 0;  // entities applied so far (a chunk may be applied over several frames)
        std::vector<EntityId> roots;
        std::set<EntityId> skipped;
    };
    void startLoad(ChunkCoord c);
    void unload(ChunkCoord c);
    bool applySome(double budgetMs);
    void rebase(Vec3 delta);

    Scene& m_scene;
    PartitionSettings m_settings;
    std::shared_ptr<ChunkSource> m_source;
    JobSystem* m_jobs;
    StreamingScheduler m_sched;
    std::map<ChunkCoord, std::vector<EntityId>> m_loaded;
    std::deque<Pending> m_ready;  // read + parsed, waiting for main-thread apply
    std::mutex m_mutex;
    std::shared_ptr<std::atomic<bool>> m_alive;
    struct InFlight {
        ChunkCoord coord;
        std::shared_ptr<Json> data;
        std::future<void> done;
    };
    std::vector<InFlight> m_inflight;
    DVec3 m_origin;
    Stats m_stats;
};

} // namespace fledge::world
