#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace borealis {

struct TaskProgress {
    std::uint64_t completed = 0;
    std::optional<std::uint64_t> total;
};

template <typename T>
class Task;

namespace detail {

struct TaskSignals {
    std::atomic<std::uint64_t> completed = 0;
    std::atomic<std::uint64_t> total = 0;
    std::atomic_bool totalKnown = false;
    std::atomic_bool cancelRequested = false;
};

bool submit_task(std::function<void(TaskSignals*)> function, std::shared_ptr<TaskSignals> signals);
void register_shutdown_hook(std::function<void()> hook);

enum class TaskStateStatus {
    Pending,
    Ready,
    Consumed,
};

template <typename T>
struct TaskState {
    using Continuation = std::function<void(std::optional<T>, std::exception_ptr)>;

    std::atomic<TaskStateStatus> status = TaskStateStatus::Pending;
    std::mutex mutex;
    std::optional<T> result;
    std::exception_ptr exception;
    Continuation continuation;
};

template <typename T>
class TaskSource {
public:
    TaskSource()
        : m_state(std::make_shared<TaskState<T>>()), m_signals(std::make_shared<TaskSignals>()) {}

    Task<T> task() const;

    void set_value(T value) const {
        typename TaskState<T>::Continuation continuation;
        {
            std::lock_guard lock{m_state->mutex};
            if (m_state->status.load(std::memory_order_relaxed) != TaskStateStatus::Pending) {
                return;
            }
            if (m_state->continuation) {
                continuation = std::move(m_state->continuation);
                m_state->status.store(TaskStateStatus::Consumed, std::memory_order_release);
            } else {
                m_state->result.emplace(std::move(value));
                m_state->status.store(TaskStateStatus::Ready, std::memory_order_release);
            }
        }
        if (continuation) {
            continuation(std::optional<T>{std::move(value)}, nullptr);
        }
    }

    void set_exception(std::exception_ptr exception) const {
        typename TaskState<T>::Continuation continuation;
        {
            std::lock_guard lock{m_state->mutex};
            if (m_state->status.load(std::memory_order_relaxed) != TaskStateStatus::Pending) {
                return;
            }
            if (m_state->continuation) {
                continuation = std::move(m_state->continuation);
                m_state->status.store(TaskStateStatus::Consumed, std::memory_order_release);
            } else {
                m_state->exception = exception;
                m_state->status.store(TaskStateStatus::Ready, std::memory_order_release);
            }
        }
        if (continuation) {
            continuation(std::nullopt, exception);
        }
    }

    std::shared_ptr<TaskSignals> signals() const { return m_signals; }

private:
    TaskSource(std::shared_ptr<TaskState<T>> state, std::shared_ptr<TaskSignals> signals)
        : m_state(std::move(state)), m_signals(std::move(signals)) {}

    std::shared_ptr<TaskState<T>> m_state;
    std::shared_ptr<TaskSignals> m_signals;

    template <typename U>
    friend class borealis::Task;
};

template <typename T>
Task<T> make_ready_task(T value);

}  // namespace detail

class TaskContext {
public:
    explicit TaskContext(detail::TaskSignals* signals) : m_signals{signals} {}

    bool cancel_requested() const noexcept {
        return m_signals->cancelRequested.load(std::memory_order_relaxed);
    }

    void report_progress(
        std::uint64_t completed, std::optional<std::uint64_t> total = {}) noexcept {
        m_signals->completed.store(completed, std::memory_order_relaxed);
        if (total) {
            m_signals->total.store(*total, std::memory_order_relaxed);
            m_signals->totalKnown.store(true, std::memory_order_release);
        }
    }

private:
    detail::TaskSignals* m_signals;
};

/** Cancels and drains work on the shared background pool. Later work restarts it lazily. */
void shutdown() noexcept;

/** Move-only handle for an async operation. Requests cancellation when dropped. */
template <typename T>
class Task {
public:
    Task() = default;

    Task(Task&& other) noexcept
        : m_state(std::move(other.m_state)), m_signals(std::move(other.m_signals)) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            cancel_if_pending();
            m_state = std::move(other.m_state);
            m_signals = std::move(other.m_signals);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { cancel_if_pending(); }

    explicit operator bool() const noexcept { return m_state != nullptr; }

    bool ready() const noexcept {
        return m_state != nullptr &&
               m_state->status.load(std::memory_order_acquire) != detail::TaskStateStatus::Pending;
    }

    /** Requests cancellation. */
    void cancel() noexcept {
        if (m_signals != nullptr) {
            m_signals->cancelRequested.store(true, std::memory_order_relaxed);
        }
    }

    /** Returns the latest progress snapshot. */
    TaskProgress progress() const noexcept {
        if (m_signals == nullptr) {
            return {};
        }

        TaskProgress progress{
            .completed = m_signals->completed.load(std::memory_order_relaxed),
        };
        if (m_signals->totalKnown.load(std::memory_order_acquire)) {
            progress.total = m_signals->total.load(std::memory_order_relaxed);
        }
        return progress;
    }

    /** Returns the completed value once. Producer exceptions are rethrown. */
    std::optional<T> try_take() {
        if (m_state == nullptr ||
            m_state->status.load(std::memory_order_acquire) != detail::TaskStateStatus::Ready)
        {
            return std::nullopt;
        }

        std::optional<T> result;
        std::exception_ptr exception;
        {
            std::lock_guard lock{m_state->mutex};
            if (m_state->status.load(std::memory_order_relaxed) != detail::TaskStateStatus::Ready) {
                return std::nullopt;
            }
            result = std::move(m_state->result);
            exception = std::move(m_state->exception);
            m_state->status.store(detail::TaskStateStatus::Consumed, std::memory_order_release);
        }
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
        return result;
    }

    /** Consumes this task and transforms its value on the thread that completes it. */
    template <typename F>
    auto map(
        F&& function) && -> Task<std::remove_cvref_t<std::invoke_result_t<std::decay_t<F>&, T&&>>> {
        using Mapper = std::decay_t<F>;
        using U = std::remove_cvref_t<std::invoke_result_t<Mapper&, T&&>>;
        static_assert(!std::is_void_v<U>, "Task::map requires a value result");

        if (m_state == nullptr) {
            return {};
        }

        auto state = std::move(m_state);
        auto signals = std::move(m_signals);
        detail::TaskSource<U> source{std::make_shared<detail::TaskState<U>>(), signals};
        Task<U> task = source.task();
        auto mapper = std::make_shared<Mapper>(std::forward<F>(function));
        typename detail::TaskState<T>::Continuation continuation =
            [source, mapper](std::optional<T> value, std::exception_ptr exception) mutable {
                if (exception != nullptr) {
                    source.set_exception(exception);
                    return;
                }
                try {
                    source.set_value(std::invoke(*mapper, std::move(*value)));
                } catch (...) {
                    source.set_exception(std::current_exception());
                }
            };

        std::optional<T> value;
        std::exception_ptr exception;
        bool runNow = false;
        {
            std::lock_guard lock{state->mutex};
            const auto status = state->status.load(std::memory_order_relaxed);
            if (status == detail::TaskStateStatus::Pending) {
                state->continuation = std::move(continuation);
            } else if (status == detail::TaskStateStatus::Ready) {
                value = std::move(state->result);
                exception = std::move(state->exception);
                state->status.store(detail::TaskStateStatus::Consumed, std::memory_order_release);
                runNow = true;
            } else {
                exception = std::make_exception_ptr(std::logic_error{"Cannot map a consumed task"});
                runNow = true;
            }
        }
        if (runNow) {
            continuation(std::move(value), exception);
        }
        return task;
    }

private:
    Task(std::shared_ptr<detail::TaskState<T>> state, std::shared_ptr<detail::TaskSignals> signals)
        : m_state(std::move(state)), m_signals(std::move(signals)) {}

    void cancel_if_pending() noexcept {
        if (m_state != nullptr &&
            m_state->status.load(std::memory_order_acquire) == detail::TaskStateStatus::Pending)
        {
            cancel();
        }
    }

    std::shared_ptr<detail::TaskState<T>> m_state;
    std::shared_ptr<detail::TaskSignals> m_signals;

    friend class detail::TaskSource<T>;
};

/** Runs cancellable work on the shared bounded worker pool. */
template <typename F>
auto spawn(F&& function)
    -> Task<std::remove_cvref_t<std::invoke_result_t<std::decay_t<F>&, TaskContext&>>> {
    using Function = std::decay_t<F>;
    using Result = std::remove_cvref_t<std::invoke_result_t<Function&, TaskContext&>>;
    static_assert(!std::is_void_v<Result>, "spawn requires a value result");

    detail::TaskSource<Result> source;
    auto task = source.task();
    auto work = [source, function = Function{std::forward<F>(function)}](
                    detail::TaskSignals* signals) mutable {
        try {
            TaskContext context{signals};
            source.set_value(std::invoke(function, context));
        } catch (...) {
            source.set_exception(std::current_exception());
        }
    };
    if (!detail::submit_task(std::move(work), source.signals())) {
        source.set_exception(std::make_exception_ptr(
            std::runtime_error{"Background worker pool could not accept the task"}));
    }
    return task;
}

namespace detail {

template <typename T>
Task<T> TaskSource<T>::task() const {
    return Task<T>{m_state, m_signals};
}

template <typename T>
Task<T> make_ready_task(T value) {
    TaskSource<T> source;
    Task<T> task = source.task();
    source.set_value(std::move(value));
    return task;
}

}  // namespace detail

}  // namespace borealis
