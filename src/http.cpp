#include "borealis/http.hpp"

#include "http_internal.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace borealis::http {
namespace detail {

struct WorkItem {
    Request request;
    borealis::detail::TaskSource<Result> source;
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
        m_requests.emplace_back(item.source.signals());
        m_queue.emplace_back(std::move(item));

        if (m_queue.size() > m_idleWorkers && m_liveWorkers < MaxWorkers && !spawn_worker() &&
            m_liveWorkers == 0)
        {
            m_queue.pop_back();
            m_requests.pop_back();
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
        while (!m_queue.empty()) {
            WorkItem item = std::move(m_queue.front());
            m_queue.pop_front();
            Result result{
                .error = Error::Canceled,
                .message = "Request canceled",
            };
            item.source.set_value(std::move(result));
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
        Worker* workerPtr = worker.get();
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

            Result result;
            auto signals = item.source.signals();
            if (signals->cancelRequested.load(std::memory_order_relaxed)) {
                result = {
                    .error = Error::Canceled,
                    .message = "Request canceled",
                };
            } else {
                try {
                    result = perform(item.request, signals.get());
                } catch (const std::exception& exception) {
                    result = {
                        .error = Error::Network,
                        .message = exception.what(),
                    };
                } catch (...) {
                    result = {
                        .error = Error::Network,
                        .message = "HTTP backend failed",
                    };
                }
            }

            item.source.set_value(std::move(result));
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::condition_variable m_workersStopped;
    std::deque<WorkItem> m_queue;
    std::vector<std::weak_ptr<borealis::detail::TaskSignals>> m_requests;
    std::list<std::unique_ptr<Worker>> m_workers;
    size_t m_liveWorkers = 0;
    size_t m_idleWorkers = 0;
    bool m_stopping = false;
};

/*
 * initialize() gates worker creation so Android JNI requests cannot predate SDL. The
 * pool itself stays empty until the first request and returns to zero when idle.
 */
std::mutex g_poolMutex;
std::unique_ptr<WorkerPool> g_pool;

Result validate_request(const Request& request) {
    if (request.url.empty()) {
        return {
            .error = Error::InvalidUrl,
            .message = "URL is empty",
        };
    }
    if (!request.url.starts_with("https://")) {
        return {
            .error = Error::UnsupportedScheme,
            .message = "Only https:// URLs are supported",
        };
    }
    return {};
}

}  // namespace detail

bool initialize() noexcept {
    std::lock_guard lock{detail::g_poolMutex};
    if (detail::g_pool != nullptr) {
        return true;
    }
    try {
        detail::g_pool = std::make_unique<detail::WorkerPool>();
        return true;
    } catch (...) {
        detail::g_pool.reset();
        return false;
    }
}

void shutdown() noexcept {
    std::unique_ptr<detail::WorkerPool> pool;
    {
        std::lock_guard lock{detail::g_poolMutex};
        pool = std::move(detail::g_pool);
    }
    if (pool != nullptr) {
        pool->stop();
    }
}

Task<Result> start(Request request) {
    if (!available()) {
        return borealis::detail::make_ready_task(Result{
            .error = Error::NoBackend,
            .message = "No HTTP backend is available",
        });
    }

    if (Result error = detail::validate_request(request); error.error != Error::None) {
        return borealis::detail::make_ready_task(std::move(error));
    }

    borealis::detail::TaskSource<Result> source;
    Task<Result> task = source.task();
    std::lock_guard lock{detail::g_poolMutex};
    if (detail::g_pool == nullptr) {
        source.set_value({
            .error = Error::NotInitialized,
            .message = "HTTP worker pool is not initialized",
        });
        return task;
    }
    if (!detail::g_pool->submit({.request = std::move(request), .source = source})) {
        source.set_value({
            .error = Error::NotInitialized,
            .message = "HTTP worker pool could not accept the request",
        });
    }
    return task;
}

}  // namespace borealis::http
