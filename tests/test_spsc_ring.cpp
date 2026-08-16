#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "engine/spsc_ring.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// SPSCRing tests. The concurrency correctness of Phase 4 rests on this
// primitive, so besides the basic capacity/order semantics there is a real
// producer/consumer stress test: 200k items through a 1024-slot ring, with
// the producer spinning on a full ring, verifying nothing is lost and the
// order is strictly preserved.
// ---------------------------------------------------------------------------

TEST(SPSCRing, PushPopPreservesOrderAndCapacity) {
    SPSCRing<int> ring(8);
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(ring.try_push(i));
    }
    EXPECT_EQ(ring.size(), 8u);
    EXPECT_FALSE(ring.try_push(99)); // full

    for (int i = 0; i < 8; ++i) {
        int v = -1;
        EXPECT_TRUE(ring.try_pop(v));
        EXPECT_EQ(v, i);
    }
    int v;
    EXPECT_FALSE(ring.try_pop(v)); // empty
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, WrapsAroundAfterDrain) {
    SPSCRing<int> ring(4);
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) {
            EXPECT_TRUE(ring.try_push(round * 10 + i));
        }
        for (int i = 0; i < 4; ++i) {
            int v;
            EXPECT_TRUE(ring.try_pop(v));
            EXPECT_EQ(v, round * 10 + i);
        }
    }
}

TEST(SPSCRing, CapacityOne) {
    SPSCRing<int> ring(1);
    EXPECT_TRUE(ring.try_push(42));
    EXPECT_FALSE(ring.try_push(43));
    int v;
    EXPECT_TRUE(ring.try_pop(v));
    EXPECT_EQ(v, 42);
    EXPECT_TRUE(ring.try_push(44));
}

TEST(SPSCRing, RejectsNonPowerOfTwoCapacity) {
    EXPECT_THROW(SPSCRing<int> ring(3), std::invalid_argument);
    EXPECT_THROW(SPSCRing<int> ring(0), std::invalid_argument);
}

TEST(SPSCRing, ConcurrentProducerConsumerNoLossNoReorder) {
    constexpr std::uint64_t kItems = 200000;
    SPSCRing<std::uint64_t> ring(1024);
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            while (!ring.try_push(i)) {
                cpuPause(); // ring full -- the consumer is behind
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t expected = 0;
    std::size_t consumed = 0;
    std::uint64_t v = 0;
    while (!producer_done.load(std::memory_order_acquire) || ring.size() > 0) {
        while (ring.try_pop(v)) {
            EXPECT_EQ(v, expected) << "order violation at item " << consumed;
            ++expected;
            ++consumed;
        }
        cpuPause();
    }
    // Final drain of anything committed just before done.
    while (ring.try_pop(v)) {
        EXPECT_EQ(v, expected);
        ++expected;
        ++consumed;
    }

    producer.join();
    EXPECT_EQ(consumed, kItems);
    EXPECT_EQ(expected, kItems);
    EXPECT_TRUE(ring.empty());
}
