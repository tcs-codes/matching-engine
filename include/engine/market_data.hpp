#pragma once

#include <cstdint>

#include "engine/spsc_ring.hpp"
#include "engine/trade.hpp"
#include "engine/types.hpp"

namespace engine {

// ---------------------------------------------------------------------------
// Market data tap (Phase 6).
//
// ConcurrentBook owns the book and only the matcher thread mutates it, so
// the matcher is the single source of truth for market data. An optional
// MarketDataSink installed at construction gets called by the matcher after
// every mutating op with the fills it produced and the current best bid/ask.
// The RingMarketDataSink forwards those calls into an SPSCRing (matcher =
// producer, one publisher thread = consumer), reusing the Phase 4 lock-free
// primitive -- the publisher never touches the book.
//
// The ring is intentionally *lossy* under overload (try_push drops when
// full): a demo feed would rather drop an old book update than stall the
// matching hot path. Production feeds would backpressure instead.
// ---------------------------------------------------------------------------

struct MarketDataEvent {
    enum class Kind : std::uint8_t {
        Trade,      // fill: a trade occurred
        BookUpdate, // best bid/ask changed (sent after every mutation)
    };
    Kind kind{Kind::BookUpdate};
    Fill fill{}; // valid when kind == Trade
    Price best_bid{0}; // 0 = no best on that side
    Price best_ask{0};
};

// Called by the matching thread. Implementations run on the hot path: they
// must be thread-safe (only the matcher calls them) and must not block.
class MarketDataSink {
public:
    virtual ~MarketDataSink() = default;
    virtual void onTrade(const Fill& fill, Price best_bid, Price best_ask) = 0;
    virtual void onBookUpdate(Price best_bid, Price best_ask) = 0;
};

// Forwards events into a lock-free SPSC ring for a publisher thread.
class RingMarketDataSink : public MarketDataSink {
public:
    explicit RingMarketDataSink(std::size_t capacity) : ring_(capacity) {}

    void onTrade(const Fill& fill, Price best_bid, Price best_ask) override {
        ring_.try_push(MarketDataEvent{MarketDataEvent::Kind::Trade, fill, best_bid, best_ask});
    }
    void onBookUpdate(Price best_bid, Price best_ask) override {
        ring_.try_push(MarketDataEvent{MarketDataEvent::Kind::BookUpdate, {}, best_bid, best_ask});
    }

    bool tryPop(MarketDataEvent& out) { return ring_.try_pop(out); }

private:
    SPSCRing<MarketDataEvent> ring_;
};

} // namespace engine
