#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "engine/concurrent_book.hpp"
#include "engine/fast_order_book.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// ConcurrentBook tests.
//
// The core claim of Phase 4: the concurrency layer changes *nothing* about
// matching semantics -- it only moves the (already single-threaded) book
// onto a dedicated thread and pipes ops/results through lock-free rings.
// Three kinds of tests back that up:
//
//   1. Basic ops -- the existing behaviors work through the round trip.
//   2. Differential vs. FastOrderBook -- a single producer thread feeds the
//      same randomized op stream to both, and every result plus the whole
//      observable state is compared after every op. With one producer, the
//      matcher applies ops in submission order, so this is deterministic.
//   3. Multi-producer invariants -- several producer threads hammer the book
//      on disjoint id ranges but overlapping prices (so they match against
//      each other), and a global volume-conservation identity must hold.
// ---------------------------------------------------------------------------

namespace {

void expectSameFills(const std::vector<Fill>& a, const std::vector<Fill>& b) {
    ASSERT_EQ(a.size(), b.size()) << "fill count divergence";
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].resting_order_id, b[i].resting_order_id) << "fill " << i;
        EXPECT_EQ(a[i].incoming_order_id, b[i].incoming_order_id) << "fill " << i;
        EXPECT_EQ(a[i].price, b[i].price) << "fill " << i;
        EXPECT_EQ(a[i].quantity, b[i].quantity) << "fill " << i;
    }
}

} // namespace

// --- 1. Basic ops through the concurrent round trip ------------------------

TEST(ConcurrentBook, BasicMatchingAndQueries) {
    ConcurrentBook book(4, 1024);

    book.addLimitOrder(1, Side::Sell, 10000, 10);
    auto fills = book.addLimitOrder(2, Side::Buy, 10000, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u);
    EXPECT_EQ(fills[0].quantity, 10);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_FALSE(book.isCrossed());

    book.stop();
    EXPECT_EQ(book.book().bidLevelCount(), 0u);
    EXPECT_EQ(book.book().askLevelCount(), 0u);
}

TEST(ConcurrentBook, PartialFillAndIOCThroughRoundTrip) {
    ConcurrentBook book(4, 1024);

    book.addLimitOrder(1, Side::Sell, 10000, 10);
    auto fills = book.addIOCOrder(2, Side::Buy, 10000, 4);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 4);
    EXPECT_TRUE(book.contains(1));
    EXPECT_EQ(book.get(1)->quantity, 6);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 6);
    EXPECT_FALSE(book.contains(2)); // IOC never rests

    // The IOC's leftover (6) was discarded, not rested -- no best bid.
    EXPECT_FALSE(book.bestBid().has_value());
    book.stop();
}

TEST(ConcurrentBook, CancelAndCancelReplaceThroughRoundTrip) {
    ConcurrentBook book(4, 1024);

    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Sell, 10000, 10);

    // Shrink preserves priority.
    auto outcome = book.cancelReplace(1, 10000, 4);
    EXPECT_EQ(outcome.result, ReplaceResult::Replaced);
    EXPECT_TRUE(outcome.preserved_priority);
    EXPECT_EQ(book.get(1)->quantity, 4);

    // Grow loses priority: order 2 is now ahead.
    auto outcome2 = book.cancelReplace(1, 10000, 20);
    EXPECT_FALSE(outcome2.preserved_priority);
    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 10);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 2u);

    // Order 2 was fully consumed by that buy; order 1 (now qty 20) rests.
    EXPECT_EQ(book.cancel(2), CancelResult::NotFound); // already filled
    EXPECT_EQ(book.cancel(1), CancelResult::Cancelled);
    book.stop();
}

TEST(ConcurrentBook, EnqueueBatchAppliedInOrder) {
    ConcurrentBook book(4, 1024);

    // Fire-and-forget a batch: 100 resting buys at 9900..9999, then 100 IOC
    // sells all at 9900 that sweep them one at a time (best bid first).
    constexpr int kOrders = 100;
    for (int i = 0; i < kOrders; ++i) {
        book.enqueue(static_cast<OrderId>(i + 1), Side::Buy, 9900 + i, 10);
    }
    for (int i = 0; i < kOrders; ++i) {
        book.enqueue(static_cast<OrderId>(kOrders + 1 + i), Side::Sell, 9900, 10,
                     OrderType::IOC);
    }
    book.waitForLastOp();

    // Applied strictly in submission order: the IOC sells swept all 100
    // resting buys, so the book is empty again.
    EXPECT_FALSE(book.isCrossed());
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_FALSE(book.bestAsk().has_value());
    book.stop();
    EXPECT_EQ(book.book().bidLevelCount(), 0u);
    EXPECT_EQ(book.book().askLevelCount(), 0u);
}

TEST(ConcurrentBook, TooManyProducerThreadsThrows) {
    ConcurrentBook book(1, 1024);
    book.addLimitOrder(1, Side::Buy, 10000, 10); // main thread claims the one slot

    std::atomic<bool> threw{false};
    std::thread extra([&] {
        try {
            book.addLimitOrder(2, Side::Buy, 10001, 10);
        } catch (const std::runtime_error&) {
            threw.store(true);
        }
    });
    extra.join();
    EXPECT_TRUE(threw.load());
    book.stop();
}

// --- 2. Differential vs. FastOrderBook (single producer) --------------------

TEST(ConcurrentBook, DifferentialVsSequentialSingleProducer) {
    FastOrderBook ref(0, 200000);
    ConcurrentBook book(8, 1024, -1, 0, 200000);

    std::mt19937 rng(777);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<Price> price_dist(9950, 10050);
    std::uniform_int_distribution<Quantity> qty_dist(1, 20);
    std::uniform_int_distribution<int> op_dist(0, 9);

    std::set<Price> touched;
    std::vector<OrderId> live_ids;
    std::set<OrderId> issued;
    OrderId next_id = 1;

    auto checkState = [&]() {
        EXPECT_EQ(book.bestBid(), ref.bestBid());
        EXPECT_EQ(book.bestAsk(), ref.bestAsk());
        EXPECT_EQ(book.isCrossed(), ref.isCrossed());
        for (Price p : touched) {
            EXPECT_EQ(book.quantityAt(Side::Buy, p), ref.quantityAt(Side::Buy, p))
                << "bid quantity at " << p;
            EXPECT_EQ(book.quantityAt(Side::Sell, p), ref.quantityAt(Side::Sell, p))
                << "ask quantity at " << p;
        }
        for (OrderId id : live_ids) {
            auto c = book.get(id);
            auto r = ref.get(id);
            ASSERT_EQ(c.has_value(), r.has_value()) << "get divergence for id " << id;
            if (c.has_value()) {
                EXPECT_EQ(c->price, r->price);
                EXPECT_EQ(c->quantity, r->quantity);
            }
        }
        for (OrderId id : issued) {
            EXPECT_EQ(book.contains(id), ref.contains(id)) << "existence divergence for id " << id;
        }
    };

    auto pruneConsumed = [&](const std::vector<Fill>& fills) {
        for (const auto& f : fills) {
            if (!ref.contains(f.resting_order_id)) {
                auto it = std::find(live_ids.begin(), live_ids.end(), f.resting_order_id);
                if (it != live_ids.end()) {
                    *it = live_ids.back();
                    live_ids.pop_back();
                }
            }
        }
    };

    constexpr int kIterations = 2000;
    for (int i = 0; i < kIterations; ++i) {
        const int op = op_dist(rng);

        if (op <= 1 && !live_ids.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live_ids[idx];

            EXPECT_EQ(book.cancel(id), ref.cancel(id));
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();

        } else if (op <= 3) {
            const OrderId id = next_id++;
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Quantity qty = qty_dist(rng);
            touched.insert(price);
            issued.insert(id);

            auto c_fills = book.addIOCOrder(id, side, price, qty);
            auto r_fills = ref.addIOCOrder(id, side, price, qty);
            expectSameFills(c_fills, r_fills);
            pruneConsumed(r_fills);

        } else if (op <= 5 && !live_ids.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live_ids[idx];
            const Price new_price = price_dist(rng);
            const Quantity new_qty = qty_dist(rng);
            touched.insert(new_price);

            auto c = book.cancelReplace(id, new_price, new_qty);
            auto r = ref.cancelReplace(id, new_price, new_qty);
            EXPECT_EQ(c.result, r.result);
            EXPECT_EQ(c.preserved_priority, r.preserved_priority);
            expectSameFills(c.fills, r.fills);

            pruneConsumed(r.fills);
            if (!ref.contains(id)) {
                auto it = std::find(live_ids.begin(), live_ids.end(), id);
                if (it != live_ids.end()) {
                    *it = live_ids.back();
                    live_ids.pop_back();
                }
            }

        } else {
            const OrderId id = next_id++;
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Quantity qty = qty_dist(rng);
            touched.insert(price);
            issued.insert(id);

            auto c_fills = book.addLimitOrder(id, side, price, qty);
            auto r_fills = ref.addLimitOrder(id, side, price, qty);
            expectSameFills(c_fills, r_fills);
            if (ref.contains(id)) {
                live_ids.push_back(id);
            }
            pruneConsumed(r_fills);
        }

        checkState();
        ASSERT_FALSE(book.isCrossed()) << "concurrent book crossed at iteration " << i;
    }

    book.stop();

    // After stop, the underlying books must agree completely (belt and
    // suspenders on top of the routed queries above).
    EXPECT_EQ(book.book().bidLevelCount(), ref.bidLevelCount());
    EXPECT_EQ(book.book().askLevelCount(), ref.askLevelCount());
    for (OrderId id : issued) {
        EXPECT_EQ(book.book().contains(id), ref.contains(id));
    }
}

// --- 3. Multi-producer invariants -------------------------------------------

TEST(ConcurrentBook, MultiProducerVolumeConservation) {
    constexpr int kProducers = 4;
    constexpr int kIterations = 3000;
    ConcurrentBook book(kProducers, 1024);

    std::atomic<Quantity> total_inserted{0};
    std::atomic<Quantity> total_filled{0};
    std::atomic<Quantity> total_cancelled{0};
    std::atomic<bool> any_crossed{false};

    auto worker = [&](int which) {
        std::mt19937 rng(1000 + which);
        std::uniform_int_distribution<int> side_dist(0, 1);
        std::uniform_int_distribution<Price> price_dist(9950, 10050);
        std::uniform_int_distribution<Quantity> qty_dist(1, 20);
        std::uniform_int_distribution<int> op_dist(0, 9);

        std::vector<OrderId> live;
        OrderId next = static_cast<OrderId>(which) * 1000000 + 1; // disjoint id ranges

        for (int i = 0; i < kIterations; ++i) {
            const int op = op_dist(rng);

            if (op <= 1 && !live.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
                const std::size_t idx = pick(rng);
                const OrderId id = live[idx];

                // The order may have been consumed (fully or partially) by
                // another producer's matching op in the meantime. The matcher
                // reports the *exact* resting quantity released, computed
                // atomically with the removal, so the accounting is exact
                // even when stale ids are cancelled.
                const OpResult& r = book.submitOp(OrderOp{OpType::Cancel, 0, id, Side::Buy, 0, 0});
                if (r.ok) {
                    total_cancelled.fetch_add(r.quantity_value, std::memory_order_relaxed);
                }
                live[idx] = live.back();
                live.pop_back();

            } else if (op <= 3) {
                const OrderId id = next++;
                const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
                const Price price = price_dist(rng);
                const Quantity qty = qty_dist(rng);

                total_inserted.fetch_add(qty, std::memory_order_relaxed);
                auto fills = book.addIOCOrder(id, side, price, qty);
                Quantity matched = 0;
                for (const auto& f : fills) matched += f.quantity;
                total_filled.fetch_add(2 * matched, std::memory_order_relaxed);
                total_cancelled.fetch_add(qty - matched, std::memory_order_relaxed); // discarded

            } else if (op <= 5 && !live.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
                const std::size_t idx = pick(rng);
                const OrderId id = live[idx];
                const Price new_price = price_dist(rng);
                const Quantity new_qty = qty_dist(rng);

                const OpResult& r = book.submitOp(
                    OrderOp{OpType::CancelReplace, 0, id, Side::Buy, new_price, new_qty});
                if (r.ok) {
                    // Released volume (exact, from the matcher): the shrink
                    // delta on the priority-preserving path, the whole old
                    // quantity on the priority-losing path.
                    total_cancelled.fetch_add(r.quantity_value, std::memory_order_relaxed);
                    if (!r.preserved_priority) {
                        total_inserted.fetch_add(new_qty, std::memory_order_relaxed);
                        Quantity matched = 0;
                        for (const auto& f : r.fills) matched += f.quantity;
                        total_filled.fetch_add(2 * matched, std::memory_order_relaxed);
                    }
                }
                // r is invalidated by the next submit -- read everything we
                // need (done above) before this contains() round trip.
                if (!book.contains(id)) {
                    auto it = std::find(live.begin(), live.end(), id);
                    if (it != live.end()) {
                        *it = live.back();
                        live.pop_back();
                    }
                }

            } else {
                const OrderId id = next++;
                const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
                const Price price = price_dist(rng);
                const Quantity qty = qty_dist(rng);

                total_inserted.fetch_add(qty, std::memory_order_relaxed);
                auto fills = book.addLimitOrder(id, side, price, qty);
                Quantity matched = 0;
                for (const auto& f : fills) matched += f.quantity;
                total_filled.fetch_add(2 * matched, std::memory_order_relaxed);

                if (book.contains(id)) {
                    live.push_back(id);
                }
            }

            if (book.isCrossed()) {
                any_crossed.store(true, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kProducers; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    book.stop();
    EXPECT_FALSE(any_crossed.load());

    // Global volume identity: every unit of inserted volume is either still
    // resting, matched (each fill consumes one unit from each side), or
    // cancelled/discarded. Verified by summing the resting book directly.
    Quantity resting = 0;
    const Quantity inserted = total_inserted.load();
    const Quantity filled = total_filled.load();
    const Quantity cancelled = total_cancelled.load();
    EXPECT_LE(filled + cancelled, inserted) << "cannot consume more than was inserted";
    EXPECT_GT(filled, 0) << "expected at least some cross-producer matching";

    // Walk every price the workers could have touched (the tight range above)
    // and sum what's actually resting.
    for (Price p = 9950; p <= 10050; ++p) {
        resting += book.book().quantityAt(Side::Buy, p);
        resting += book.book().quantityAt(Side::Sell, p);
    }
    EXPECT_EQ(resting, inserted - filled - cancelled);
}
