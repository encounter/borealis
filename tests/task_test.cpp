#include "borealis/task.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

TEST(Task, PublishesValueOnce) {
    borealis::detail::TaskSource<int> source;
    borealis::Task<int> task = source.task();
    EXPECT_FALSE(task.ready());
    EXPECT_FALSE(task.try_take().has_value());

    source.set_value(42);
    EXPECT_TRUE(task.ready());
    EXPECT_EQ(task.try_take(), 42);
    EXPECT_FALSE(task.try_take().has_value());
}

TEST(Task, MapsPendingValue) {
    borealis::detail::TaskSource<int> source;
    auto task = source.task().map([](int value) { return std::to_string(value); });
    EXPECT_FALSE(task.ready());

    source.set_value(42);
    EXPECT_TRUE(task.ready());
    EXPECT_EQ(task.try_take(), "42");
}

TEST(Task, MappingCanRaceCompletion) {
    for (int i = 0; i < 1000; ++i) {
        borealis::detail::TaskSource<int> source;
        auto sourceTask = source.task();
        std::thread producer{[source, i] { source.set_value(i); }};
        auto mapped = std::move(sourceTask).map([](int value) { return value + 1; });
        producer.join();

        ASSERT_TRUE(mapped.ready());
        EXPECT_EQ(mapped.try_take(), i + 1);
    }
}

TEST(Task, SharesProgressAndCancellationAcrossMap) {
    borealis::detail::TaskSource<int> source;
    auto signals = source.signals();
    auto task = source.task().map([](int value) { return value; });

    signals->completed.store(4, std::memory_order_relaxed);
    signals->total.store(10, std::memory_order_relaxed);
    signals->totalKnown.store(true, std::memory_order_release);
    const borealis::TaskProgress progress = task.progress();
    EXPECT_EQ(progress.completed, 4);
    ASSERT_TRUE(progress.total.has_value());
    EXPECT_EQ(*progress.total, 10);

    task.cancel();
    EXPECT_TRUE(signals->cancelRequested.load(std::memory_order_relaxed));
}

TEST(Task, DestructionRequestsCancellation) {
    borealis::detail::TaskSource<int> source;
    auto signals = source.signals();
    {
        borealis::Task<int> task = source.task();
        EXPECT_FALSE(signals->cancelRequested.load(std::memory_order_relaxed));
    }
    EXPECT_TRUE(signals->cancelRequested.load(std::memory_order_relaxed));
}

TEST(Task, PreservesUnexpectedExceptions) {
    borealis::detail::TaskSource<int> source;
    auto task = source.task().map([](int) -> int { throw std::runtime_error{"failed"}; });
    source.set_value(1);
    ASSERT_TRUE(task.ready());
    EXPECT_THROW(task.try_take(), std::runtime_error);
    EXPECT_FALSE(task.try_take().has_value());
}

TEST(Task, SpawnRunsOnSharedPoolAndReportsProgress) {
    auto task = borealis::spawn([](borealis::TaskContext& context) {
        context.report_progress(3, 5);
        return 42;
    });
    while (!task.ready()) {
        std::this_thread::yield();
    }
    const auto progress = task.progress();
    EXPECT_EQ(progress.completed, 3u);
    EXPECT_EQ(progress.total, 5u);
    EXPECT_EQ(task.try_take(), 42);
    borealis::shutdown();
}

TEST(Task, ShutdownDrainsAndAllowsLazyRestart) {
    std::atomic_bool started = false;
    auto canceled = borealis::spawn([&started](borealis::TaskContext& context) {
        started.store(true, std::memory_order_release);
        while (!context.cancel_requested()) {
            std::this_thread::yield();
        }
        return 1;
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    borealis::shutdown();
    ASSERT_TRUE(canceled.ready());
    EXPECT_EQ(canceled.try_take(), 1);

    auto restarted = borealis::spawn([](borealis::TaskContext&) { return 2; });
    while (!restarted.ready()) {
        std::this_thread::yield();
    }
    EXPECT_EQ(restarted.try_take(), 2);
    borealis::shutdown();
}

}  // namespace
