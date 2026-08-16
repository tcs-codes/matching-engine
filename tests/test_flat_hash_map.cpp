#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>

#include "engine/flat_hash_map.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// FlatHashMap tests (the Phase 6 cancel-index container).
//
// The map is not a general-purpose container -- it exists to answer
// find/insert/erase for the order-id -> pool-index index. The strong test
// here is differential: thousands of random insert/erase cycles (a
// cancel-heavy book is *the* adversarial workload for open addressing, which
// is exactly why erase uses backward-shift rather than tombstones) compared
// against std::unordered_map after every op. Plus focused cases for the
// collision and wrap-around paths.
// ---------------------------------------------------------------------------

namespace {

// Sequential ids hash into low table slots; multiples of 64 share low bits;
// both stress probing. The differential test uses all of them.
std::uint64_t mixIds(std::uint64_t i) {
    switch (i % 3) {
        case 0: return i + 1;          // sequential
        case 1: return i * 64;         // low-bit collisions
        case 2: return (i * 64) ^ 0x9e3779b97f4a7c15ull; // arbitrary-ish
    }
    return i;
}

} // namespace

TEST(FlatHashMap, InsertFindEraseBasics) {
    FlatHashMap<std::uint64_t, std::int32_t, IdHash> map;
    EXPECT_TRUE(map.empty());

    map[42] = 7;
    map[43] = 8;
    EXPECT_EQ(map.size(), 2u);
    ASSERT_NE(map.find(42), nullptr);
    EXPECT_EQ(*map.find(42), 7);
    ASSERT_NE(map.find(43), nullptr);
    EXPECT_EQ(*map.find(43), 8);
    EXPECT_TRUE(map.contains(42));
    EXPECT_FALSE(map.contains(99));

    EXPECT_TRUE(map.erase(42));
    EXPECT_FALSE(map.erase(42)); // already gone
    EXPECT_EQ(map.size(), 1u);
    EXPECT_FALSE(map.contains(42));
    EXPECT_EQ(*map.find(43), 8); // sibling unaffected
}

TEST(FlatHashMap, OverwriteUpdatesValue) {
    FlatHashMap<std::uint64_t, std::int32_t, IdHash> map;
    map[1] = 100;
    map[1] = 200;
    EXPECT_EQ(map.size(), 1u);
    EXPECT_EQ(*map.find(1), 200);
}

TEST(FlatHashMap, GrowsPastInitialCapacity) {
    FlatHashMap<std::uint64_t, std::int32_t, IdHash> map; // starts at 8 slots
    for (std::uint64_t i = 0; i < 100000; ++i) {
        map[i] = static_cast<std::int32_t>(i);
    }
    EXPECT_EQ(map.size(), 100000u);
    for (std::uint64_t i = 0; i < 100000; ++i) {
        ASSERT_NE(map.find(i), nullptr);
        EXPECT_EQ(*map.find(i), static_cast<std::int32_t>(i));
    }
}

TEST(FlatHashMap, ReserveAvoidsRehash) {
    FlatHashMap<std::uint64_t, std::int32_t, IdHash> map;
    map.reserve(100000);
    for (std::uint64_t i = 0; i < 100000; ++i) {
        map[i] = 1;
    }
    EXPECT_EQ(map.size(), 100000u);
}

TEST(FlatHashMap, BackwardShiftAfterHeavyEraseKeepsEverythingFindable) {
    // Force a dense table, then delete every other key; every survivor must
    // still be reachable from its home slot (this is what tombstones would
    // otherwise poison).
    FlatHashMap<std::uint64_t, std::int32_t, IdHash> map;
    map.reserve(1 << 12);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        map[i] = static_cast<std::int32_t>(i);
    }
    for (std::uint64_t i = 0; i < 1000; i += 2) {
        EXPECT_TRUE(map.erase(i));
    }
    EXPECT_EQ(map.size(), 500u);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        if (i % 2 == 0) {
            EXPECT_FALSE(map.contains(i));
        } else {
            ASSERT_NE(map.find(i), nullptr) << "key " << i << " unreachable after backward shift";
            EXPECT_EQ(*map.find(i), static_cast<std::int32_t>(i));
        }
    }
}

TEST(FlatHashMap, DifferentialVsUnorderedMap) {
    FlatHashMap<std::uint64_t, std::int32_t, IdHash> mine;
    std::unordered_map<std::uint64_t, std::int32_t> ref;

    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> op_dist(0, 9); // 0-6 insert, 7-9 erase
    std::uniform_int_distribution<std::uint64_t> key_dist(1, 3000); // small range => collisions

    for (int iter = 0; iter < 200000; ++iter) {
        const std::uint64_t key = mixIds(key_dist(rng));
        const int op = op_dist(rng);

        if (op <= 6) {
            const std::int32_t v = static_cast<std::int32_t>(iter);
            mine[key] = v;
            ref[key] = v;
        } else {
            ASSERT_EQ(mine.erase(key), ref.erase(key) != 0) << "erase divergence at " << iter;
        }

        ASSERT_EQ(mine.size(), ref.size()) << "size divergence at " << iter;
        const bool m = mine.contains(key);
        const bool r = ref.contains(key);
        ASSERT_EQ(m, r) << "contains divergence at " << iter << " for key " << key;
        if (m) {
            ASSERT_EQ(*mine.find(key), ref.at(key)) << "value divergence at " << iter;
        }
    }

    // Final state agrees completely.
    ASSERT_EQ(mine.size(), ref.size());
    for (const auto& [k, v] : ref) {
        ASSERT_NE(mine.find(k), nullptr) << "missing key " << k;
        EXPECT_EQ(*mine.find(k), v);
    }
}

TEST(FlatHashMap, EmptyAndMissingKeyQueries) {
    FlatHashMap<std::uint64_t, std::int32_t, IdHash> map;
    EXPECT_EQ(map.find(1), nullptr);
    EXPECT_FALSE(map.contains(1));
    EXPECT_FALSE(map.erase(1));
}
