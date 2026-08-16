#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "engine/order_book.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// Direct invariant checks
// ---------------------------------------------------------------------------

TEST(Invariant, EmptyBookIsNotCrossed) {
    OrderBook book;
    EXPECT_FALSE(book.isCrossed());
}

TEST(Invariant, OneSidedBookIsNotCrossed) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 10000, 10);
    EXPECT_FALSE(book.isCrossed());
}

TEST(Invariant, NonCrossingTwoSidedBookIsNotCrossed) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 9900, 10);
    book.addLimitOrder(2, Side::Sell, 10000, 10);
    EXPECT_FALSE(book.isCrossed());
    EXPECT_LT(*book.bestBid(), *book.bestAsk());
}

TEST(Invariant, MatchingEngineNeverLeavesBookCrossed) {
    // A limit order priced to cross should match immediately rather than
    // rest and cross the book. This is really a test of addLimitOrder's
    // matching-before-resting behavior, phrased as an invariant check.
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 5);

    // This buy is priced *above* the resting ask, so if it were naively
    // inserted without matching first, the book would be crossed.
    book.addLimitOrder(2, Side::Buy, 10050, 10);

    EXPECT_FALSE(book.isCrossed());
    // Remainder (10 - 5 = 5) rests at the buy's limit price.
    EXPECT_EQ(book.bestBid(), 10050);
}

// ---------------------------------------------------------------------------
// Randomized stress test (lightweight property-based test)
// ---------------------------------------------------------------------------
//
// This isn't a full property-based framework (no shrinking), but it fills
// the same role for Phase 1: throw a large number of pseudo-random valid
// operations at the book and assert invariants hold after every single one.
// Fixed seed keeps it deterministic and reproducible on failure.

namespace {

struct StressStats {
    Quantity total_inserted = 0;
    Quantity total_filled = 0;
    Quantity total_cancelled = 0;
};

} // namespace

TEST(Invariant, RandomizedOperationsNeverCrossBookAndConserveVolume) {
    OrderBook book;
    std::mt19937 rng(42); // fixed seed: deterministic, reproducible failures
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<Price> price_dist(9950, 10050); // tight range to force crossing
    std::uniform_int_distribution<Quantity> qty_dist(1, 20);
    // 0-1: cancel, 2-3: IOC, 4-5: cancel-replace, 6-9: add limit.
    std::uniform_int_distribution<int> op_dist(0, 9);

    StressStats stats;
    std::vector<OrderId> live_ids;
    OrderId next_id = 1;

    auto pruneIfConsumed = [&](const std::vector<Fill>& fills) {
        // Fills can fully consume *resting* orders on the opposite side of
        // whatever operation triggered them -- keep live_ids honest.
        for (const auto& f : fills) {
            if (!book.contains(f.resting_order_id)) {
                auto it = std::find(live_ids.begin(), live_ids.end(), f.resting_order_id);
                if (it != live_ids.end()) {
                    *it = live_ids.back();
                    live_ids.pop_back();
                }
            }
        }
    };

    constexpr int kIterations = 5000;
    for (int i = 0; i < kIterations; ++i) {
        const int op = op_dist(rng);

        if (op <= 1 && !live_ids.empty()) {
            // --- cancel ---
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            std::size_t idx = pick(rng);
            OrderId id = live_ids[idx];

            auto maybe_order = book.get(id);
            auto result = book.cancel(id);
            if (result == CancelResult::Cancelled) {
                ASSERT_TRUE(maybe_order.has_value());
                stats.total_cancelled += maybe_order->quantity;
            }
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();

        } else if (op <= 3) {
            // --- IOC ---
            OrderId id = next_id++;
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            Price price = price_dist(rng);
            Quantity qty = qty_dist(rng);

            stats.total_inserted += qty;
            auto fills = book.addIOCOrder(id, side, price, qty);

            Quantity filled_this_op = 0;
            for (const auto& f : fills) filled_this_op += f.quantity;
            stats.total_filled += 2 * filled_this_op;
            // Whatever didn't match is discarded, not resting -- same
            // accounting bucket as an explicit cancel.
            stats.total_cancelled += (qty - filled_this_op);

            ASSERT_FALSE(book.contains(id)); // IOC must never rest
            pruneIfConsumed(fills);

        } else if (op <= 5 && !live_ids.empty()) {
            // --- cancel-replace ---
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            std::size_t idx = pick(rng);
            OrderId id = live_ids[idx];

            auto before = book.get(id);
            ASSERT_TRUE(before.has_value());
            Price new_price = price_dist(rng);
            Quantity new_qty = qty_dist(rng);

            auto outcome = book.cancelReplace(id, new_price, new_qty);
            ASSERT_EQ(outcome.result, ReplaceResult::Replaced);

            if (outcome.preserved_priority) {
                // Pure in-place shrink: the delta is volume leaving the
                // book without a trade, same as a partial cancel.
                stats.total_cancelled += (before->quantity - new_qty);
            } else {
                // Implemented as cancel(old) + addOrder(new): the old
                // resting quantity is fully released, and new_qty is fresh
                // inserted volume that then goes through matching.
                stats.total_cancelled += before->quantity;
                stats.total_inserted += new_qty;

                Quantity filled_this_op = 0;
                for (const auto& f : outcome.fills) filled_this_op += f.quantity;
                stats.total_filled += 2 * filled_this_op;
                pruneIfConsumed(outcome.fills);
            }

            if (!book.contains(id)) {
                auto it = std::find(live_ids.begin(), live_ids.end(), id);
                if (it != live_ids.end()) {
                    *it = live_ids.back();
                    live_ids.pop_back();
                }
            }

        } else {
            // --- add limit ---
            OrderId id = next_id++;
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            Price price = price_dist(rng);
            Quantity qty = qty_dist(rng);

            stats.total_inserted += qty;
            auto fills = book.addLimitOrder(id, side, price, qty);

            // Each fill of quantity Q removes Q units of *inserted* volume
            // from the resting order AND Q units from the incoming order --
            // two separately-inserted quantities are consumed per trade, so
            // this counts double the raw fill quantity.
            Quantity filled_this_op = 0;
            for (const auto& f : fills) filled_this_op += f.quantity;
            stats.total_filled += 2 * filled_this_op;

            if (book.contains(id)) {
                live_ids.push_back(id);
            }
            pruneIfConsumed(fills);
        }

        // The core invariant, checked after every single mutation.
        ASSERT_FALSE(book.isCrossed()) << "book crossed at iteration " << i;
    }

    // Volume conservation: everything inserted is either still resting,
    // was matched (removed via fill), or was cancelled/discarded. We don't
    // track "still resting" precisely via the counters above (that requires
    // walking the book), so first verify the weaker but still meaningful
    // property: matched + cancelled volume never exceeds inserted volume,
    // and something actually happened (the test isn't vacuous).
    EXPECT_LE(stats.total_filled + stats.total_cancelled, stats.total_inserted);
    EXPECT_GT(stats.total_filled, 0) << "expected at least some matches given the tight price range";

    // Cross-check total resting quantity against inserted - filled - cancelled
    // by walking the book directly (independent of the running counters above).
    Quantity resting_in_book = 0;
    for (auto id : live_ids) {
        auto o = book.get(id);
        ASSERT_TRUE(o.has_value());
        resting_in_book += o->quantity;
    }
    EXPECT_EQ(resting_in_book, stats.total_inserted - stats.total_filled - stats.total_cancelled);
}

