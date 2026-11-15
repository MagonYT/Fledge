#include "fledge/core/jobs.h"

#include <algorithm>
#include <atomic>

namespace fledge {

JobSystem::JobSystem(unsigned threads) {
    if (threads == 0) {
        const unsigned logicalProcessors = std::thread::hardware_concurrency();
        threads = logicalProcessors > 1 ? logicalProcessors - 1 : 1;
    }
    for (unsigned i = 0; i < threads; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_workers) t.join();
}

std::future<void> JobSystem::submit(std::function<void()> job) {
    std::packaged_task<void()> task(std::move(job));
    auto fut = task.get_future();
    {
        std::lock_guard lock(m_mutex);
        m_queue.push(std::move(task));
    }
    m_cv.notify_one();
    return fut;
}

void JobSystem::parallelFor(size_t count, const std::function<void(size_t)>& fn) {
    if (count == 0) return;
    std::atomic<size_t> next{0};
    size_t n = std::min<size_t>(count, m_workers.size() + 1);
    std::vector<std::future<void>> futures;
    auto body = [&] {
        for (size_t i = next++; i < count; i = next++) fn(i);
    };
    for (size_t w = 1; w < n; ++w) futures.push_back(submit(body));
    body(); // calling thread participates
    for (auto& f : futures) f.get();
}

void JobSystem::workerLoop() {
    for (;;) {
        std::packaged_task<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop();
        }
        task();
    }
}

} // namespace fledge
