#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "engine/fast_order_book.hpp"
#include "engine/order_book.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// Differential fuzz test: FastOrderBook vs. OrderBook baseline.
//
// This is the core correctness argument for Phase 3. The optimized book is
// *not* a new matching algorithm -- it's the same algorithm over different
// data structures -- so the strongest possible test is to feed both books
// byte-identical operations and demand they agree on every observable output
// after every single operation:
//
//   * the Fill stream produced by each operation (size, order, and every
//     field of every fill -- catches diverging match order or trade prices),
//   * the operation results (cancel / cancel-replace outcomes),
//   * best bid / best ask, level counts, and isCrossed,
//   * quantity and order count at every price the test ever touched,
//   * existence (contains) of *every id ever issued* -- this catches a book
//     forgetting to erase a filled/cancelled order from its location index,
//   * price and remaining quantity of every live order.
//
// Any divergence between the two books is a Phase 3 bug, full stop.
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

TEST(Differential, FastBookMatchesBaselineAcrossRandomOpStream) {
    OrderBook baseline;
    FastOrderBook fast(0, 200000);

    std::mt19937 rng(2024); // fixed seed: deterministic, reproducible failures
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<Price> price_dist(9950, 10050); // tight range to force crossing
    std::uniform_int_distribution<Quantity> qty_dist(1, 20);
    // 0-1: cancel, 2-3: IOC, 4-5: cancel-replace, 6-9: add limit.
    std::uniform_int_distribution<int> op_dist(0, 9);

    std::set<Price> touched; // every price ever used, for quantity/count checks
    std::vector<OrderId> live_ids;
    std::set<OrderId> issued; // every id ever issued, for existence checks
    OrderId next_id = 1;

    auto checkAgreement = [&]() {
        EXPECT_EQ(fast.bestBid(), baseline.bestBid());
        EXPECT_EQ(fast.bestAsk(), baseline.bestAsk());
        EXPECT_EQ(fast.isCrossed(), baseline.isCrossed());
        EXPECT_EQ(fast.bidLevelCount(), baseline.bidLevelCount());
        EXPECT_EQ(fast.askLevelCount(), baseline.askLevelCount());

        for (Price p : touched) {
            EXPECT_EQ(fast.quantityAt(Side::Buy, p), baseline.quantityAt(Side::Buy, p))
                << "bid quantity at " << p;
            EXPECT_EQ(fast.quantityAt(Side::Sell, p), baseline.quantityAt(Side::Sell, p))
                << "ask quantity at " << p;
            EXPECT_EQ(fast.orderCountAt(Side::Buy, p), baseline.orderCountAt(Side::Buy, p))
                << "bid order count at " << p;
            EXPECT_EQ(fast.orderCountAt(Side::Sell, p), baseline.orderCountAt(Side::Sell, p))
                << "ask order count at " << p;
        }

        for (OrderId id : live_ids) {
            ASSERT_EQ(fast.contains(id), baseline.contains(id)) << "contains divergence for id " << id;
            auto f = fast.get(id);
            auto b = baseline.get(id);
            ASSERT_EQ(f.has_value(), b.has_value()) << "get divergence for id " << id;
            if (f.has_value()) {
                EXPECT_EQ(f->price, b->price) << "price divergence for id " << id;
                EXPECT_EQ(f->quantity, b->quantity) << "quantity divergence for id " << id;
            }
        }

        // Existence of every id ever issued, live or not. This is the check
        // that catches a location index out of sync (e.g. an erased id that
        // the optimized book forgot to drop, or one it dropped too early).
        for (OrderId id : issued) {
            EXPECT_EQ(fast.contains(id), baseline.contains(id)) << "existence divergence for id " << id;
        }
    };

    // Fills can fully consume *resting* orders on the opposite side of
    // whatever operation triggered them -- keep live_ids honest. Tracking is
    // driven by the baseline book (the reference of truth); checkAgreement
    // then asserts the optimized book agrees with it.
    auto pruneConsumed = [&](const std::vector<Fill>& fills) {
        for (const auto& f : fills) {
            if (!baseline.contains(f.resting_order_id)) {
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
            const std::size_t idx = pick(rng);
            const OrderId id = live_ids[idx];

            EXPECT_EQ(fast.cancel(id), baseline.cancel(id));

            live_ids[idx] = live_ids.back();
            live_ids.pop_back();

        } else if (op <= 3) {
            // --- IOC ---
            const OrderId id = next_id++;
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Quantity qty = qty_dist(rng);
            touched.insert(price);
            issued.insert(id);

            auto f_fills = fast.addIOCOrder(id, side, price, qty);
            auto b_fills = baseline.addIOCOrder(id, side, price, qty);
            expectSameFills(f_fills, b_fills);

            EXPECT_FALSE(fast.contains(id)); // IOC never rests
            EXPECT_FALSE(baseline.contains(id));
            pruneConsumed(b_fills);

        } else if (op <= 5 && !live_ids.empty()) {
            // --- cancel-replace ---
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live_ids[idx];

            const Price new_price = price_dist(rng);
            const Quantity new_qty = qty_dist(rng);
            touched.insert(new_price);

            auto f_outcome = fast.cancelReplace(id, new_price, new_qty);
            auto b_outcome = baseline.cancelReplace(id, new_price, new_qty);

            EXPECT_EQ(f_outcome.result, b_outcome.result);
            EXPECT_EQ(f_outcome.preserved_priority, b_outcome.preserved_priority);
            expectSameFills(f_outcome.fills, b_outcome.fills);

            pruneConsumed(b_outcome.fills);
            if (!baseline.contains(id)) {
                auto it = std::find(live_ids.begin(), live_ids.end(), id);
                if (it != live_ids.end()) {
                    *it = live_ids.back();
                    live_ids.pop_back();
                }
            }

        } else {
            // --- add limit ---
            const OrderId id = next_id++;
            const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Quantity qty = qty_dist(rng);
            touched.insert(price);
            issued.insert(id);

            auto f_fills = fast.addLimitOrder(id, side, price, qty);
            auto b_fills = baseline.addLimitOrder(id, side, price, qty);
            expectSameFills(f_fills, b_fills);

            if (baseline.contains(id)) {
                live_ids.push_back(id);
            }
            pruneConsumed(b_fills);
        }

        checkAgreement();
        ASSERT_FALSE(fast.isCrossed()) << "fast book crossed at iteration " << i;
    }
}
