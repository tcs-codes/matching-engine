#include <gtest/gtest.h>

#include <vector>

#include "engine/concurrent_book.hpp"
#include "engine/market_data.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// Market data tap tests. The matcher is the single source of truth, so the
// sink must observe, in submission order, exactly the fills each op produces
// plus the resulting best bid/ask. Ops are submitted from one thread, so the
// event stream is deterministic.
// ---------------------------------------------------------------------------

namespace {

std::vector<MarketDataEvent> drain(RingMarketDataSink& sink) {
    std::vector<MarketDataEvent> out;
    MarketDataEvent e;
    while (sink.tryPop(e)) {
        out.push_back(e);
    }
    return out;
}

} // namespace

TEST(MarketData, SinkSeesTradesAndBookUpdatesInOrder) {
    RingMarketDataSink sink(4096);
    ConcurrentBook book(1, 1024, -1, 0, 200000, 1u << 20, &sink);

    // 1) A resting ask: book update only (bid none, ask 10000).
    book.addLimitOrder(1, Side::Sell, 10000, 10);

    // 2) A crossing buy of 5: trade (5 @ 10000) + book update.
    book.addLimitOrder(2, Side::Buy, 10000, 5);

    // 3) A cancel of the remaining ask: book update (bid 10000 resting,
    //    ask gone).
    book.cancel(1);

    book.stop();

    const auto events = drain(sink);
    // Total: add (update) + buy (trade + update) + cancel (update) = 4.
    ASSERT_EQ(events.size(), 4u);

    // Event 0: the add rested -- book update only.
    EXPECT_EQ(events[0].kind, MarketDataEvent::Kind::BookUpdate);
    EXPECT_EQ(events[0].best_bid, 0);
    EXPECT_EQ(events[0].best_ask, 10000);

    // Event 1: the trade.
    EXPECT_EQ(events[1].kind, MarketDataEvent::Kind::Trade);
    EXPECT_EQ(events[1].fill.price, 10000);
    EXPECT_EQ(events[1].fill.quantity, 5);
    EXPECT_EQ(events[1].fill.resting_order_id, 1u);
    EXPECT_EQ(events[1].fill.incoming_order_id, 2u);

    // Event 2: book update after the trade (buy 5 fully consumed by the ask,
    // so no resting bid; ask has 5 left).
    EXPECT_EQ(events[2].kind, MarketDataEvent::Kind::BookUpdate);
    EXPECT_EQ(events[2].best_bid, 0);
    EXPECT_EQ(events[2].best_ask, 10000);

    // Event 3: the cancel of the remaining ask -- no trade, book update.
    EXPECT_EQ(events[3].kind, MarketDataEvent::Kind::BookUpdate);
    EXPECT_EQ(events[3].best_bid, 0);
    EXPECT_EQ(events[3].best_ask, 0);
}

TEST(MarketData, SweepProducesOneTradePerFill) {
    RingMarketDataSink sink(4096);
    ConcurrentBook book(1, 1024, -1, 0, 200000, 1u << 20, &sink);

    book.addLimitOrder(1, Side::Sell, 10000, 5);
    book.addLimitOrder(2, Side::Sell, 10010, 5);
    book.addLimitOrder(3, Side::Buy, 10020, 10); // sweeps both asks

    book.stop();

    const auto events = drain(sink);
    std::size_t trades = 0;
    std::vector<Price> trade_prices;
    for (const auto& e : events) {
        if (e.kind == MarketDataEvent::Kind::Trade) {
            ++trades;
            trade_prices.push_back(e.fill.price);
        }
    }
    ASSERT_EQ(trades, 2u);
    EXPECT_EQ(trade_prices[0], 10000); // best first
    EXPECT_EQ(trade_prices[1], 10010);
}

TEST(MarketData, IOCAndCancelProduceNoStaleTrades) {
    RingMarketDataSink sink(4096);
    ConcurrentBook book(1, 1024, -1, 0, 200000, 1u << 20, &sink);

    book.addLimitOrder(1, Side::Sell, 10000, 10);
    book.addIOCOrder(2, Side::Buy, 10000, 3); // partial fill, 7 discarded
    book.cancel(1);                            // cancel the remaining 7

    book.stop();

    const auto events = drain(sink);
    std::size_t trades = 0;
    std::size_t book_updates = 0;
    for (const auto& e : events) {
        if (e.kind == MarketDataEvent::Kind::Trade) ++trades;
        if (e.kind == MarketDataEvent::Kind::BookUpdate) ++book_updates;
    }
    // Exactly one trade (the IOC fill); the cancel must NOT re-emit the
    // previous op's stale fills, and must still emit a book update.
    EXPECT_EQ(trades, 1u);
    EXPECT_EQ(book_updates, 3u); // add, IOC, cancel
}
