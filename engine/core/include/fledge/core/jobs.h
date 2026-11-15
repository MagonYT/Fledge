#pragma once
// Job system - STUB-LEVEL: a plain fixed-size thread pool with a single
// locked queue plus parallelFor. Good enough for asset import / packaging.
// Roadmap: work-stealing deques, fibers, job dependencies/counters.
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fledge {

class JobSystem {
public:
    explicit JobSystem(unsigned threads = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    std::future<void> submit(std::function<void()> job);
    // Runs fn(i) for i in [0, count) across workers and waits.
    void parallelFor(size_t count, const std::function<void(size_t)>& fn);
    unsigned workerCount() const { return static_cast<unsigned>(m_workers.size()); }

private:
    void workerLoop();
    std::vector<std::thread> m_workers;
    std::queue<std::packaged_task<void()>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
};

} // namespace fledge
