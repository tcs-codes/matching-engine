#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <random>
#include <vector>

#include "engine/concurrent_book.hpp"

namespace engine {

// ---------------------------------------------------------------------------
// OpStreamGenerator -- deterministic synthetic order flow (Phase 5).
//
// Produces a stream of OrderOp with the properties that make an order book
// interesting to measure:
//
//   * price locality: orders cluster near a mid price that performs a slow
//     random walk (this is what real flow looks like -- the book is deep
//     near the touch and sparse elsewhere);
//   * mostly passive: the majority of adds rest on the book;
//   * occasionally aggressive: a configurable fraction of adds are priced to
//     cross the opposite side, and IOC orders sweep liquidity;
//   * cancels and cancel-replaces target ids the generator believes are
//     live (its own view -- the book may have filled some of them, in which
//     case the cancel is a harmless NotFound).
//
// Same seed -> identical stream, guaranteed (mt19937, no external state).
// ---------------------------------------------------------------------------
class OpStreamGenerator {
public:
    struct Config {
        std::uint64_t seed{1};
        std::size_t count{100000};
        Price mid{10000};
        Price spread{50};       // orders mostly within +/-spread of mid
        Quantity min_qty{1};
        Quantity max_qty{20};
        double aggressive{0.15}; // fraction of adds priced to cross
    };

    explicit OpStreamGenerator(Config cfg);

    // Next op, or nullopt once `count` ops have been produced. `seq` is left
    // 0 (ConcurrentBook::submitOp stamps it).
    std::optional<OrderOp> next();

private:
    Config cfg_;
    std::mt19937 rng_;
    Price mid_;
    std::vector<OrderId> live_;
    OrderId next_id_{1};
    std::size_t produced_{0};

    OrderId takeLiveId();
};

// ---------------------------------------------------------------------------
// Text op log format (for dumping / replaying streams):
//
//   # engine op log v1
//   L <id> <side:0=buy,1=sell> <price> <qty>   add limit
//   I <id> <side> <price> <qty>                 add IOC
//   C <id>                                      cancel
//   R <id> <new_price> <new_qty>                cancel-replace
//
// '#' starts a comment; blank lines ignored. Side is 0/1 to keep parsing
// trivial and unambiguous. dumpOps/loadOps are exact inverses.
// ---------------------------------------------------------------------------
void dumpOps(std::ostream& out, const std::vector<OrderOp>& ops);
std::vector<OrderOp> loadOps(std::istream& in);

} // namespace engine
