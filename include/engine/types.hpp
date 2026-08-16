#pragma once

#include <cstdint>

namespace engine {

// --- Fundamental aliases ----------------------------------------------------
//
// Price is represented as an integer number of "ticks" (e.g. cents), not a
// double. Floating point prices are a classic correctness bug in matching
// engines: 100.10 - 100.00 doesn't reliably equal 0.10 in binary floating
// point, and price levels are map keys that must compare exactly. Integer
// ticks sidestep the whole problem. Callers decide what a tick represents
// (cents, 1/10000ths, etc.) at the I/O boundary.
using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;

// Monotonically increasing sequence number assigned at insertion time.
// This is what gives us time priority within a price level: two orders at
// the same price are ranked by Sequence, not by wall-clock time (which is
// coarser than insertion order and not guaranteed monotonic across calls).
using Sequence = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

inline Side opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Phase 1 supports plain resting limit orders. Phase 2 adds IOC (Immediate-
// Or-Cancel): match as much as possible immediately against the book, and
// discard whatever quantity is left over instead of resting it.
enum class OrderType : std::uint8_t {
    Limit,
    IOC,
};

} // namespace engine
