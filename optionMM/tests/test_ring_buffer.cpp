#include <gtest/gtest.h>
#include "common/ring_buffer.h"

#include <thread>
#include <atomic>
#include <cstdint>

using namespace omm;

// ─── Basic single-threaded correctness ───────────────────────────────────────

TEST(RingBuffer, BasicPushPop) {
    SPSCRingBuffer<int, 16> buf;
    EXPECT_TRUE(buf.try_push(42));
    int val = 0;
    EXPECT_TRUE(buf.try_pop(val));
    EXPECT_EQ(val, 42);
}

TEST(RingBuffer, EmptyPopReturnsFalse) {
    SPSCRingBuffer<int, 8> buf;
    int val = 0;
    EXPECT_FALSE(buf.try_pop(val));
}

TEST(RingBuffer, FullPushReturnsFalse) {
    // Capacity - 1 usable slots (one reserved)
    SPSCRingBuffer<int, 8> buf;
    for (int i = 0; i < 7; ++i)
        EXPECT_TRUE(buf.try_push(i));
    // Buffer is now full (7 of 7 usable slots occupied)
    EXPECT_FALSE(buf.try_push(99));
}

TEST(RingBuffer, OrderPreserved) {
    SPSCRingBuffer<int, 32> buf;
    for (int i = 0; i < 10; ++i)
        ASSERT_TRUE(buf.try_push(i));
    for (int i = 0; i < 10; ++i) {
        int val = -1;
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, i);
    }
}

TEST(RingBuffer, WrapAround) {
    // Push/pop more items than the ring capacity to exercise wrap-around
    SPSCRingBuffer<uint64_t, 16> buf;
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        // Keep buffer from filling: push one, pop one
        ASSERT_TRUE(buf.try_push(static_cast<uint64_t>(i)));
        uint64_t val = 0;
        ASSERT_TRUE(buf.try_pop(val));
        EXPECT_EQ(val, static_cast<uint64_t>(i));
    }
}

TEST(RingBuffer, CapacityConstant) {
    SPSCRingBuffer<int, 64> buf;
    EXPECT_EQ(buf.capacity(), 63u);  // Capacity - 1 usable slots
}

TEST(RingBuffer, SizeApprox) {
    SPSCRingBuffer<int, 16> buf;
    EXPECT_EQ(buf.size_approx(), 0u);
    ASSERT_TRUE(buf.try_push(1));
    ASSERT_TRUE(buf.try_push(2));
    EXPECT_EQ(buf.size_approx(), 2u);
    int v;
    ASSERT_TRUE(buf.try_pop(v));
    EXPECT_EQ(buf.size_approx(), 1u);
}

// ─── Non-trivially-large type ─────────────────────────────────────────────────

struct Payload {
    uint64_t a, b, c, d;
};

TEST(RingBuffer, LargerType) {
    SPSCRingBuffer<Payload, 8> buf;
    Payload p{1, 2, 3, 4};
    ASSERT_TRUE(buf.try_push(p));
    Payload out{};
    ASSERT_TRUE(buf.try_pop(out));
    EXPECT_EQ(out.a, 1u);
    EXPECT_EQ(out.b, 2u);
    EXPECT_EQ(out.c, 3u);
    EXPECT_EQ(out.d, 4u);
}

// ─── SPSC concurrent correctness ─────────────────────────────────────────────

TEST(RingBuffer, SPSCConcurrent) {
    constexpr int N = 100'000;
    SPSCRingBuffer<uint64_t, 1024> buf;
    std::atomic<bool> start{false};
    uint64_t sum_produced = 0;
    uint64_t sum_consumed = 0;

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) spin_pause();
        for (int i = 1; i <= N; ++i) {
            while (!buf.try_push(static_cast<uint64_t>(i)))
                spin_pause();
            sum_produced += static_cast<uint64_t>(i);
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) spin_pause();
        uint64_t received = 0;
        while (received < N) {
            uint64_t val = 0;
            if (buf.try_pop(val)) {
                sum_consumed += val;
                ++received;
            } else {
                spin_pause();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    EXPECT_EQ(sum_produced, sum_consumed);
    // Expected sum: N*(N+1)/2
    EXPECT_EQ(sum_consumed, static_cast<uint64_t>(N) * (N + 1) / 2);
}

// ─── Cache-line separation of head and tail ───────────────────────────────────
// This is a structural test — verifies the buffer layout prevents false sharing.
TEST(RingBuffer, CacheLineSeparation) {
    // The ring buffer template doesn't expose offsets, but we can check that
    // sizeof SPSCRingBuffer with a tiny element is larger than 128 bytes
    // (two cache lines minimum for head + tail + some buffer slots).
    SPSCRingBuffer<uint8_t, 2> buf;
    EXPECT_GE(sizeof(buf), 128u);
}
