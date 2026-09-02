#include "borealis/task.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <thread>
#include <vector>

namespace borealis::detail {
namespace {

struct WorkItem {
    std::function<void(TaskSignals*)> function;
    std::shared_ptr<TaskSignals> signals;
};

class WorkerPool {
public:
    ~WorkerPool() { stop(); }

    bool submit(WorkItem item) {
        std::lock_guard lock{m_mutex};
        if (m_stopping) {
            return false;
        }
        reap_workers();
        std::erase_if(m_requests, [](const auto& request) { return request.expired(); });
        m_requests.emplace_back(item.signals);
        m_queue.emplace_back(std::move(item));
        if (m_queue.size() > m_idleWorkers && m_liveWorkers < MaxWorkers && !spawn_worker() &&
            m_liveWorkers == 0)
        {
            m_queue.pop_back();
            return false;
        }
        m_condition.notify_one();
        return true;
    }

    void stop() noexcept {
        std::unique_lock lock{m_mutex};
        if (m_stopping) {
            return;
        }
        m_stopping = true;
        for (const auto& request : m_requests) {
            if (const auto signals = request.lock()) {
                signals->cancelRequested.store(true, std::memory_order_relaxed);
            }
        }
        m_condition.notify_all();
        m_workersStopped.wait(lock, [this] { return m_liveWorkers == 0; });
        lock.unlock();
        for (const auto& worker : m_workers) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        m_workers.clear();
    }

private:
    static constexpr size_t MaxWorkers = 8;
    static constexpr auto IdleTimeout = std::chrono::seconds{30};

    struct Worker {
        std::thread thread;
        std::atomic_bool done = false;
    };

    bool spawn_worker() {
        auto worker = std::make_unique<Worker>();
        auto* workerPtr = worker.get();
        m_workers.emplace_back(std::move(worker));
        ++m_liveWorkers;
        try {
            workerPtr->thread = std::thread{[this, workerPtr] { worker_main(workerPtr); }};
        } catch (...) {
            --m_liveWorkers;
            m_workers.pop_back();
            return false;
        }
        return true;
    }

    void reap_workers() {
        for (auto iter = m_workers.begin(); iter != m_workers.end();) {
            if (!(*iter)->done.load(std::memory_order_acquire)) {
                ++iter;
                continue;
            }
            if ((*iter)->thread.joinable()) {
                (*iter)->thread.join();
            }
            iter = m_workers.erase(iter);
        }
    }

    void retire_worker(Worker* worker) {
        --m_liveWorkers;
        worker->done.store(true, std::memory_order_release);
        m_workersStopped.notify_all();
    }

    void worker_main(Worker* worker) {
        for (;;) {
            WorkItem item;
            {
                std::unique_lock lock{m_mutex};
                ++m_idleWorkers;
                const bool ready = m_condition.wait_for(
                    lock, IdleTimeout, [this] { return m_stopping || !m_queue.empty(); });
                --m_idleWorkers;
                if ((m_stopping && m_queue.empty()) || (!ready && m_queue.empty())) {
                    retire_worker(worker);
                    return;
                }
                if (m_queue.empty()) {
                    continue;
                }
                item = std::move(m_queue.front());
                m_queue.pop_front();
            }
            item.function(item.signals.get());
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::condition_variable m_workersStopped;
    std::deque<WorkItem> m_queue;
    std::vector<std::weak_ptr<TaskSignals>> m_requests;
    std::list<std::unique_ptr<Worker>> m_workers;
    size_t m_liveWorkers = 0;
    size_t m_idleWorkers = 0;
    bool m_stopping = false;
};

std::mutex g_poolMutex;
std::unique_ptr<WorkerPool> g_pool;
bool g_shutdownInProgress = false;

}  // namespace

void shutdown_task_pool() noexcept {
    std::unique_ptr<WorkerPool> pool;
    {
        std::lock_guard lock{g_poolMutex};
        if (g_shutdownInProgress) {
            return;
        }
        g_shutdownInProgress = true;
        pool = std::move(g_pool);
    }
    if (pool != nullptr) {
        pool->stop();
    }
    {
        std::lock_guard lock{g_poolMutex};
        g_shutdownInProgress = false;
    }
}

bool submit_task(std::function<void(TaskSignals*)> function, std::shared_ptr<TaskSignals> signals) {
    std::lock_guard lock{g_poolMutex};
    if (g_shutdownInProgress) {
        return false;
    }
    try {
        if (g_pool == nullptr) {
            g_pool = std::make_unique<WorkerPool>();
        }
        return g_pool->submit({std::move(function), std::move(signals)});
    } catch (...) {
        return false;
    }
}

}  // namespace borealis::detail

namespace borealis {

void shutdown() noexcept {
    detail::shutdown_task_pool();
}

}  // namespace borealis
