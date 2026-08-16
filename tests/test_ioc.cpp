#include <gtest/gtest.h>

#include "engine/order_book.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// IOC (Immediate-Or-Cancel)
// ---------------------------------------------------------------------------

TEST(IOC, FullyFillsAndLeavesNothingResting) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 10);

    auto fills = book.addIOCOrder(2, Side::Buy, 10000, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 10);
    EXPECT_FALSE(book.contains(2)); // never rests, even though fully filled here anyway
    EXPECT_FALSE(book.bestAsk().has_value());
}

TEST(IOC, PartialFillDiscardsRemainderInsteadOfResting) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 4); // only 4 available

    auto fills = book.addIOCOrder(2, Side::Buy, 10000, 10); // wants 10

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].quantity, 4);
    EXPECT_FALSE(book.contains(2)); // remaining 6 is discarded, not resting
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_FALSE(book.bestAsk().has_value()); // resting sell was fully consumed
}

TEST(IOC, NoMatchProducesNoFillsAndNothingRests) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10100, 10); // doesn't cross 10000

    auto fills = book.addIOCOrder(2, Side::Buy, 10000, 10);

    EXPECT_TRUE(fills.empty());
    EXPECT_FALSE(book.contains(2));
    EXPECT_FALSE(book.bestBid().has_value()); // IOC never rested
    EXPECT_TRUE(book.contains(1));            // untouched resting sell remains
}

TEST(IOC, SweepsMultipleLevelsThenDiscardsLeftover) {
    OrderBook book;
    book.addLimitOrder(1, Side::Sell, 10000, 5);
    book.addLimitOrder(2, Side::Sell, 10010, 5);

    auto fills = book.addIOCOrder(3, Side::Buy, 10010, 20); // only 10 available total

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].price, 10000);
    EXPECT_EQ(fills[1].price, 10010);
    EXPECT_FALSE(book.contains(3));
    EXPECT_FALSE(book.bestAsk().has_value());
}

TEST(IOC, DoesNotAffectRestingOrdersItDoesNotCross) {
    OrderBook book;
    book.addLimitOrder(1, Side::Buy, 9900, 10);

    book.addIOCOrder(2, Side::Sell, 10000, 5); // doesn't cross 9900

    EXPECT_TRUE(book.contains(1));
    EXPECT_EQ(book.quantityAt(Side::Buy, 9900), 10);
}
