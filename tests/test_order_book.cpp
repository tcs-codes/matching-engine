#include <gtest/gtest.h>

#include "engine/order_book.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// Price-time priority
// ---------------------------------------------------------------------------

TEST(PriceTimePriority, BetterPriceMatchesFirst) {
    OrderBook book;
    // Two resting asks at different prices; incoming buy should hit the
    // cheaper one first even though it was inserted second.
    book.addLimitOrder(1, Side::Sell, 10100, 10); // worse price, inserted first
    book.addLimitOrder(2, Side::Sell, 10000, 10); // better price, inserted second

    auto fills = book.addLimitOrder(3, Side::Buy, 10100, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 2u); // the better-priced order, not the first-inserted one
    EXPECT_EQ(fills[0].price, 10000);
}

TEST(PriceTimePriority, SamePriceMatchesInInsertionOrder) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 5);  // first in time at this price
    book.addLimitOrder(2, Side::Sell, 10000, 5);  // second in time at this price

    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 5);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u); // FIFO: earlier order fills first
}

TEST(PriceTimePriority, SweepsMultipleLevelsInPriceOrder) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 5);
    book.addLimitOrder(2, Side::Sell, 10010, 5);
    book.addLimitOrder(3, Side::Sell, 10020, 5);

    // Big buy that sweeps all three levels.
    auto fills = book.addLimitOrder(4, Side::Buy, 10020, 15);

    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].price, 10000);
    EXPECT_EQ(fills[1].price, 10010);
    EXPECT_EQ(fills[2].price, 10020);
    EXPECT_FALSE(book.bestAsk().has_value());
}

// ---------------------------------------------------------------------------
// Partial fills
// ---------------------------------------------------------------------------

TEST(PartialFill, RestingOrderShrinksAndStaysAtFrontOfQueue) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);

    auto fills = book.addLimitOrder(2, Side::Buy, 10000, 4);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 4);

    // Resting order should still exist with reduced quantity.
    ASSERT_TRUE(book.contains(1));
    auto resting = book.get(1);
    ASSERT_TRUE(resting.has_value());
    EXPECT_EQ(resting->quantity, 6);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 6);
    EXPECT_EQ(book.orderCountAt(Side::Sell, 10000), 1u);
}

TEST(PartialFill, IncomingOrderRestsWithRemainderAfterPartialMatch) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 4);

    auto fills = book.addLimitOrder(2, Side::Buy, 10000, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 4);
    EXPECT_FALSE(book.contains(1)); // fully filled, removed

    ASSERT_TRUE(book.contains(2));
    auto remainder = book.get(2);
    ASSERT_TRUE(remainder.has_value());
    EXPECT_EQ(remainder->quantity, 6); // 10 - 4 rests on the book
    EXPECT_EQ(book.bestBid(), 10000);
}

TEST(PartialFill, PreservesTimePriorityAfterPartialFill) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10); // partially filled below
    book.addLimitOrder(2, Side::Sell, 10000, 10); // untouched, inserted second

    book.addLimitOrder(3, Side::Buy, 10000, 4); // partially fills order 1 only

    // Order 1 (now qty 6) should still be ahead of order 2 in the queue.
    auto fills = book.addLimitOrder(4, Side::Buy, 10000, 6);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u);
    EXPECT_FALSE(book.contains(1)); // now fully filled (6 remaining, 6 matched)
    EXPECT_TRUE(book.contains(2));  // still resting, untouched
}

// ---------------------------------------------------------------------------
// Full fills
// ---------------------------------------------------------------------------

TEST(FullFill, ExactQuantityMatchRemovesBothOrders) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);

    auto fills = book.addLimitOrder(2, Side::Buy, 10000, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 10);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(2)); // incoming fully filled, never rested
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_FALSE(book.bestBid().has_value());
}

TEST(FullFill, NonCrossingOrderRestsWithoutFilling) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10100, 10);

    auto fills = book.addLimitOrder(2, Side::Buy, 10000, 10); // doesn't cross

    EXPECT_TRUE(fills.empty());
    EXPECT_TRUE(book.contains(1));
    EXPECT_TRUE(book.contains(2));
    EXPECT_EQ(book.bestBid(), 10000);
    EXPECT_EQ(book.bestAsk(), 10100);
}

// ---------------------------------------------------------------------------
// Cancel behavior
// ---------------------------------------------------------------------------

TEST(Cancel, RemovesRestingOrder) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 10000, 10);

    auto result = book.cancel(1);

    EXPECT_EQ(result, CancelResult::Cancelled);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.bestBid().has_value());
}

TEST(Cancel, UnknownOrderReturnsNotFound) {
    OrderBook book;
    auto result = book.cancel(999);
    EXPECT_EQ(result, CancelResult::NotFound);
}

TEST(Cancel, AlreadyFilledOrderCannotBeCancelledAgain) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Buy, 10000, 10); // fully fills order 1

    EXPECT_EQ(book.cancel(1), CancelResult::NotFound);
}

TEST(Cancel, RemovesEmptyPriceLevel) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 10000, 10);
    EXPECT_EQ(book.bidLevelCount(), 1u);

    book.cancel(1);

    EXPECT_EQ(book.bidLevelCount(), 0u);
}

TEST(Cancel, LeavesOtherOrdersAtSameLevelIntact) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 10000, 10);
    book.addLimitOrder(2, Side::Buy, 10000, 5);

    book.cancel(1);

    EXPECT_TRUE(book.contains(2));
    EXPECT_EQ(book.quantityAt(Side::Buy, 10000), 5);
    EXPECT_EQ(book.orderCountAt(Side::Buy, 10000), 1u);
}

TEST(Cancel, PartiallyFilledThenCancelledRemovesRemainder) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Buy, 10000, 4); // order 1 now has qty 6

    EXPECT_EQ(book.cancel(1), CancelResult::Cancelled);
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 0);
}
