#include <gtest/gtest.h>
#include "perf/CpuPinning.hpp"
#include "perf/ArenaAllocator.hpp"
#include "perf/SpscQueue.hpp"
#include "perf/MpmcQueue.hpp"
#include "perf/SimdOps.hpp"
#include "perf/CachelinePadded.hpp"
#include <vector>
#include <thread>

using namespace trade_bot;

TEST(PerfTest, CpuPinning) {
    // Should at least not crash. Success depends on OS/permissions.
    bool result = pin_thread_to_cpu(0);
    // On some CI it might fail, so we don't ASSERT_TRUE.
    (void)result;
}

TEST(PerfTest, ArenaAllocator) {
    ArenaAllocator arena(1024);
    std::pmr::vector<int> v(arena.resource());
    v.push_back(1);
    v.push_back(2);
    EXPECT_EQ(v.size(), 2);
    arena.reset();
}

TEST(PerfTest, SpscQueue) {
    SpscQueue<int, 64> queue;
    EXPECT_TRUE(queue.push(10));
    int val = 0;
    EXPECT_TRUE(queue.pop(val));
    EXPECT_EQ(val, 10);
}

TEST(PerfTest, MpmcQueue) {
    MpmcQueue<int> queue;
    queue.push(42);
    int val = 0;
    EXPECT_TRUE(queue.try_pop(val));
    EXPECT_EQ(val, 42);
}

TEST(PerfTest, SimdSum) {
    std::vector<double> data = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    double result = SimdOps::sum(data.data(), data.size());
    EXPECT_DOUBLE_EQ(result, 36.0);
}

TEST(PerfTest, SimdDotProduct) {
    std::vector<double> a = {1.0, 2.0, 3.0, 4.0};
    std::vector<double> b = {1.0, 1.0, 1.0, 1.0};
    double result = SimdOps::dot_product(a.data(), b.data(), a.size());
    EXPECT_DOUBLE_EQ(result, 10.0);
}

TEST(PerfTest, CachelinePadded) {
    CachelinePadded<int> padded;
    padded.value = 10;
    EXPECT_EQ(padded.value, 10);
    EXPECT_EQ(alignof(decltype(padded)), 64);
}
