#include <gtest/gtest.h>

#include "engine/order_book.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// Cancel-Replace: pure size reduction preserves priority
// ---------------------------------------------------------------------------

TEST(CancelReplace, QuantityDecreaseAtSamePricePreservesPriority) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10); // first in queue
    book.addLimitOrder(2, Side::Sell, 10000, 10); // second in queue

    auto outcome = book.cancelReplace(1, 10000, 4); // shrink order 1

    EXPECT_EQ(outcome.result, ReplaceResult::Replaced);
    EXPECT_TRUE(outcome.preserved_priority);
    EXPECT_TRUE(outcome.fills.empty());
    EXPECT_EQ(book.get(1)->quantity, 4);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 14); // 4 + 10

    // Order 1 should still be ahead of order 2 despite the modify.
    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 4);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u);
}

TEST(CancelReplace, QuantityDecreaseNeverGeneratesFills) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 10000, 10);
    book.addLimitOrder(2, Side::Sell, 10050, 5); // doesn't cross either way

    auto outcome = book.cancelReplace(1, 10000, 3);

    EXPECT_TRUE(outcome.preserved_priority);
    EXPECT_TRUE(outcome.fills.empty());
    EXPECT_FALSE(book.isCrossed());
}

// ---------------------------------------------------------------------------
// Cancel-Replace: price change or quantity increase loses priority
// ---------------------------------------------------------------------------

TEST(CancelReplace, PriceChangeLosesPriorityAndMovesLevels) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);

    auto outcome = book.cancelReplace(1, 10010, 10); // same qty, different price

    EXPECT_EQ(outcome.result, ReplaceResult::Replaced);
    EXPECT_FALSE(outcome.preserved_priority);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 0);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10010), 10);
    EXPECT_EQ(book.bestAsk(), 10010);
}

TEST(CancelReplace, QuantityIncreaseAtSamePriceLosesPriority) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10); // originally first
    book.addLimitOrder(2, Side::Sell, 10000, 10); // originally second

    auto outcome = book.cancelReplace(1, 10000, 20); // grow order 1 -- should lose priority

    EXPECT_FALSE(outcome.preserved_priority);
    EXPECT_EQ(book.quantityAt(Side::Sell, 10000), 30); // 20 + 10

    // Order 2 should now be ahead of order 1 in the queue.
    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 10);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 2u);
}

TEST(CancelReplace, ReplaceThatNowCrossesMatchesImmediately) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 9900, 10);   // the order we'll replace
    book.addLimitOrder(2, Side::Sell, 10000, 10); // resting ask

    // Move order 1's price up so it now crosses the resting ask.
    auto outcome = book.cancelReplace(1, 10000, 10);

    EXPECT_FALSE(outcome.preserved_priority);
    ASSERT_EQ(outcome.fills.size(), 1u);
    EXPECT_EQ(outcome.fills[0].resting_order_id, 2u);
    EXPECT_EQ(outcome.fills[0].incoming_order_id, 1u);
    EXPECT_FALSE(book.contains(1)); // fully filled by the replace
    EXPECT_FALSE(book.contains(2));
    EXPECT_FALSE(book.isCrossed());
}

TEST(CancelReplace, ReplaceThatPartiallyFillsRestsRemainderWithFreshPriority) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Buy, 9900, 20); // will be replaced

    auto outcome = book.cancelReplace(2, 10000, 20); // now crosses, only 10 available

    ASSERT_EQ(outcome.fills.size(), 1u);
    EXPECT_EQ(outcome.fills[0].quantity, 10);
    EXPECT_TRUE(book.contains(2));
    EXPECT_EQ(book.get(2)->quantity, 10); // 20 - 10 rests
    EXPECT_EQ(book.bestBid(), 10000);
    EXPECT_FALSE(book.isCrossed());
}

// ---------------------------------------------------------------------------
// Cancel-Replace: edge cases
// ---------------------------------------------------------------------------

TEST(CancelReplace, UnknownOrderReturnsNotFound) {
    OrderBook book;
    auto outcome = book.cancelReplace(999, 10000, 10);

    EXPECT_EQ(outcome.result, ReplaceResult::NotFound);
    EXPECT_TRUE(outcome.fills.empty());
}

TEST(CancelReplace, SamePriceSameQuantityIsANoOpThatPreservesPriority) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addLimitOrder(2, Side::Sell, 10000, 10);

    auto outcome = book.cancelReplace(1, 10000, 10); // identical price/qty

    EXPECT_TRUE(outcome.preserved_priority);
    auto fills = book.addLimitOrder(3, Side::Buy, 10000, 10);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order_id, 1u); // still first
}

TEST(CancelReplace, DoesNotAffectOtherOrdersAtOtherLevels) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 9900, 10);
    book.addLimitOrder(2, Side::Buy, 9800, 5);

    book.cancelReplace(1, 9950, 10);

    EXPECT_TRUE(book.contains(2));
    EXPECT_EQ(book.quantityAt(Side::Buy, 9800), 5);
}
