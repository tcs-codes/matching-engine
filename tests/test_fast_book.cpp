#include <gtest/gtest.h>

#include <stdexcept>

#include "engine/fast_order_book.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// FastOrderBook unit tests.
//
// These are not the primary correctness proof for FastOrderBook -- that's
// tests/test_differential.cpp, which feeds an identical randomized op stream
// to both books and asserts full agreement after every operation. This file
// exists so a failing differential test can be traced back to a specific,
// human-readable scenario, and so the optimized book stands on its own in
// case the baseline is ever deleted.
// ---------------------------------------------------------------------------

// --- Price-time priority ---------------------------------------------------

TEST(FastBook, BetterPriceMatchesFirst) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10100, 10); // worse price, inserted first
    book.addLimitOrder(2, Side::Sell, 10000, 10); // better price, inserted second

    auto fills = book.addLimitOrder(3, Side::Buy, 10100, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 2u);
    EXPECT_EQ(fills[0].price, 10000);
}

TEST(FastBook, SamePriceMatchesInInsertionOrder) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 5);
    book.addLimitOrder(2, Side::Sell, 10000, 5);

    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 5);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u); // FIFO within a level
}

TEST(FastBook, SweepsMultipleLevelsInPriceOrder) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 5);
    book.addLimitOrder(2, Side::Sell, 10010, 5);
    book.addLimitOrder(3, Side::Sell, 10020, 5);

    auto fills = book.addLimitOrder(4, Side::Buy, 10020, 15);

    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].price, 10000);
    EXPECT_EQ(fills[1].price, 10010);
    EXPECT_EQ(fills[2].price, 10020);
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_EQ(book.askLevelCount(), 0u);
}

// --- Partial fills ---------------------------------------------------------

TEST(FastBook, RestingOrderShrinksAndStaysAtFrontOfQueue) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);

    auto fills = book.addLimitOrder(2, Side::Buy, 10000, 4);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 4);
    ASSERT_TRUE(book.contains(1));
    EXPECT_EQ(book.get(1)->quantity, 6);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 6);
    EXPECT_EQ(book.orderCountAt(Side::Sell, 10000), 1u);
}

TEST(FastBook, IncomingRestsWithRemainderAfterPartialMatch) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 4);

    auto fills = book.addLimitOrder(2, Side::Buy, 10000, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 4);
    EXPECT_FALSE(book.contains(1));
    ASSERT_TRUE(book.contains(2));
    EXPECT_EQ(book.get(2)->quantity, 6);
    EXPECT_EQ(book.bestBid(), 10000);
}

TEST(FastBook, PreservesTimePriorityAfterPartialFill) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Sell, 10000, 10);

    book.addLimitOrder(3, Side::Buy, 10000, 4); // partially fills order 1 only

    auto fills = book.addLimitOrder(4, Side::Buy, 10000, 6);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u); // order 1 kept its queue position
    EXPECT_FALSE(book.contains(1));
    EXPECT_TRUE(book.contains(2));
}

// --- IOC -------------------------------------------------------------------

TEST(FastBook, IOCPartialFillDiscardsRemainderInsteadOfResting) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 4);

    auto fills = book.addIOCOrder(2, Side::Buy, 10000, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 4);
    EXPECT_FALSE(book.contains(2)); // remaining 6 discarded, never rests
    EXPECT_FALSE(book.bestAsk().has_value());
}

TEST(FastBook, IOCNoMatchProducesNoFillsAndNothingRests) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10100, 10);

    auto fills = book.addIOCOrder(2, Side::Buy, 10000, 10);

    EXPECT_TRUE(fills.empty());
    EXPECT_FALSE(book.contains(2));
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_TRUE(book.contains(1));
}

// --- Cancel ----------------------------------------------------------------

TEST(FastBook, CancelRemovesOrderAndEmptyLevel) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Buy, 10000, 10);
    EXPECT_EQ(book.bidLevelCount(), 1u);

    EXPECT_EQ(book.cancel(1), CancelResult::Cancelled);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_EQ(book.bidLevelCount(), 0u);
}

TEST(FastBook, CancelUnknownAndFilledOrdersReturnNotFound) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Buy, 10000, 10); // fully fills order 1

    EXPECT_EQ(book.cancel(999), CancelResult::NotFound);
    EXPECT_EQ(book.cancel(1), CancelResult::NotFound);
}

TEST(FastBook, CancelPartialFillRemovesRemainderAndAdvancesBestBid) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Buy, 9900, 10);
    book.addLimitOrder(2, Side::Buy, 10000, 10); // best bid
    book.addLimitOrder(3, Side::Sell, 10100, 4); // partially fills order 2

    EXPECT_EQ(book.cancel(2), CancelResult::Cancelled);
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.bestBid(), 9900); // cursor advanced past the emptied level
    EXPECT_EQ(book.quantityAt(Side::Buy, 10000), 0);
}

// --- Cancel-Replace --------------------------------------------------------

TEST(FastBook, CancelReplaceShrinkPreservesPriority) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Sell, 10000, 10);

    auto outcome = book.cancelReplace(1, 10000, 4);

    EXPECT_EQ(outcome.result, ReplaceResult::Replaced);
    EXPECT_TRUE(outcome.preserved_priority);
    EXPECT_TRUE(outcome.fills.empty());
    EXPECT_EQ(book.get(1)->quantity, 4);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 14);

    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 4);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u); // still ahead of order 2
}

TEST(FastBook, CancelReplacePriceChangeLosesPriorityAndMovesLevels) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);

    auto outcome = book.cancelReplace(1, 10010, 10);

    EXPECT_FALSE(outcome.preserved_priority);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 0);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10010), 10);
    EXPECT_EQ(book.bestAsk(), 10010);
}

TEST(FastBook, CancelReplaceGrowLosesPriorityAndRequeues) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Sell, 10000, 10);

    auto outcome = book.cancelReplace(1, 10000, 20); // grow -- loses priority

    EXPECT_FALSE(outcome.preserved_priority);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 30);

    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 10);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 2u); // order 2 now ahead of order 1
}

TEST(FastBook, CancelReplaceThatNowCrossesMatchesImmediately) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Buy, 9900, 10);
    book.addLimitOrder(2, Side::Sell, 10000, 10);

    auto outcome = book.cancelReplace(1, 10000, 10);

    EXPECT_FALSE(outcome.preserved_priority);
    ASSERT_EQ(outcome.fills.size(), 1u);
    EXPECT_EQ(outcome.fills[0].resting_order_id, 2u);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.isCrossed());
}

TEST(FastBook, CancelReplaceUnknownReturnsNotFound) {
    FastOrderBook book;
    auto outcome = book.cancelReplace(999, 10000, 10);
    EXPECT_EQ(outcome.result, ReplaceResult::NotFound);
}

// --- Invariant / edge cases ------------------------------------------------

TEST(FastBook, CrossingAddNeverLeavesBookCrossed) {
    FastOrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 5);

    book.addLimitOrder(2, Side::Buy, 10050, 10); // priced above the ask

    EXPECT_FALSE(book.isCrossed());
    EXPECT_EQ(book.bestBid(), 10050); // remainder rests at the buy's limit
}

TEST(FastBook, OutOfRangeQueriesReturnZero) {
    FastOrderBook book(0, 100000);
    book.addLimitOrder(1, Side::Buy, 10000, 10);

    EXPECT_EQ(book.quantityAt(Side::Buy, -5), 0);
    EXPECT_EQ(book.quantityAt(Side::Sell, 200000), 0);
    EXPECT_EQ(book.orderCountAt(Side::Buy, -5), 0u);
    EXPECT_EQ(book.bestBid(), 10000); // in-range orders unaffected
}

TEST(FastBook, PoolExhaustionThrows) {
    // Tiny arena: proves the pool is fixed-capacity, not growing.
    FastOrderBook book(0, 100000, /*order_capacity=*/2);
    book.addLimitOrder(1, Side::Buy, 10000, 10);
    book.addLimitOrder(2, Side::Buy, 10001, 10);

    EXPECT_THROW(book.addLimitOrder(3, Side::Buy, 10002, 10), std::runtime_error);
}

TEST(FastBook, PoolSlotsAreReusedAfterFills) {
    // A fully-filled order returns its node to the pool; the same arena can
    // then serve new orders without growing. (Capacity 1: every add below is
    // a reuse of the same slot.)
    FastOrderBook book(0, 100000, /*order_capacity=*/1);
    for (OrderId i = 1; i <= 100; ++i) {
        book.addLimitOrder(i, Side::Sell, 10000, 10);
        auto fills = book.addLimitOrder(i + 1000, Side::Buy, 10000, 10);
        ASSERT_EQ(fills.size(), 1u);
        EXPECT_FALSE(book.contains(i));
        EXPECT_FALSE(book.contains(i + 1000));
    }
}
