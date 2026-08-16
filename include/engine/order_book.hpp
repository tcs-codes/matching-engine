#pragma once

#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "engine/order.hpp"
#include "engine/trade.hpp"
#include "engine/types.hpp"

namespace engine {

// A single price level: the FIFO queue of resting orders at that price,
// plus a running total so callers can query depth without walking the list.
struct PriceLevel {
    std::list<Order> orders;
    Quantity total_quantity{0};
};

// ---------------------------------------------------------------------------
// OrderBook — Phase 1: correctness-first, single-threaded.
//
// CONTAINER CHOICES AND WHY (Phase 1)
// ------------------------------------
// * std::map<Price, PriceLevel> for each side.
//     - Gives us price levels in sorted order for free (bids descending,
//       asks ascending), which is exactly what matching needs: "best price
//       first". begin() is always the best price.
//     - O(log n) insert/erase of a price level, O(log n) lookup. For Phase 1
//       this is entirely fine -- correctness and clarity first. Phase 3
//       replaces this with a flat, price-indexed array (prices are integers
//       in a bounded range near the touch, so direct indexing beats a tree)
//       once we've proven correctness and have benchmarks to justify it.
//
// * std::list<Order> for the FIFO queue at each price level, NOT std::deque.
//     - We need cancel-by-order-id to be O(1) once we've found the order,
//       which means holding a stable iterator into the queue from a hash
//       map (see `locations_` below). std::deque invalidates *all*
//       iterators on push_front/push_back even though references stay
//       valid, so storing a deque iterator across a later insert is
//       undefined behavior. std::list iterators remain valid across
//       insertion/erasure anywhere else in the list, which is exactly the
//       guarantee we need. The tradeoff is worse cache locality than a
//       deque/array -- that's precisely the kind of thing Phase 3's
//       intrusive-list-in-a-pool-allocator design fixes, once we can
//       benchmark the improvement instead of guessing at it.
//
// * std::unordered_map<OrderId, OrderLocation> for O(1) cancel lookup.
//     - Cancel-by-id is a hard requirement (Phase 2 needs it for
//       cancel-replace too), and without this index cancel would require
//       scanning every price level -- O(n). This is the classic
//       time/space tradeoff: an extra index kept in sync on every mutation,
//       in exchange for O(1) average lookup.
//
// WHAT DOESN'T CHANGE LATER
// ---------------------------
// The *matching logic* (price-time priority, partial fill handling, the
// shape of Fill events) is a Phase 1 concern and should NOT need to change
// in later phases -- only the data structures underneath it get swapped out
// for speed. Keeping matching logic decoupled from container choice here is
// intentional, so Phase 3's rewrite is a data-structure swap, not a
// rewrite of correctness-critical logic we've already tested.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    // Adds an order of the given type, matching against the opposite side
    // first. Returns the list of fills generated (empty if nothing crossed).
    //
    // - Limit: any unfilled remainder rests on the book at `price`.
    // - IOC (Immediate-Or-Cancel): matches as much as possible right now;
    //   whatever quantity is left over is discarded, never rests. An IOC
    //   order is never findable via contains()/get() after this call
    //   returns, even if fully unfilled.
    std::vector<Fill> addOrder(OrderId id, Side side, Price price, Quantity quantity,
                                OrderType type = OrderType::Limit);

    // Convenience wrapper -- Phase 1 call sites and tests keep working
    // unchanged.
    std::vector<Fill> addLimitOrder(OrderId id, Side side, Price price, Quantity quantity) {
        return addOrder(id, side, price, quantity, OrderType::Limit);
    }

    // Convenience wrapper for an Immediate-Or-Cancel order.
    std::vector<Fill> addIOCOrder(OrderId id, Side side, Price price, Quantity quantity) {
        return addOrder(id, side, price, quantity, OrderType::IOC);
    }

    // Cancels a resting order by id. Returns NotFound if the order is
    // unknown or has already fully filled/cancelled.
    CancelResult cancel(OrderId id);

    // Modifies a resting order's price and/or quantity in place, keeping
    // the same order id.
    //
    // Time-priority rule (matches common exchange behavior): priority is
    // PRESERVED only when the price is unchanged and the new quantity is
    // less than or equal to the current quantity (a pure size reduction).
    // Any price change, or any quantity increase, is treated as losing
    // priority: the order is removed and re-inserted as if newly submitted
    // -- it gets a fresh sequence number, goes to the back of its price
    // level's queue, and is matched against the opposite side again (so it
    // may fill immediately if the new price now crosses). This mirrors why
    // real exchanges do it this way: without it, a resting order could
    // "grow" its size for free while keeping its place in line, or hop
    // between price levels without ever losing queue position.
    //
    // Returns NotFound (with no side effects) if `id` doesn't exist.
    CancelReplaceOutcome cancelReplace(OrderId id, Price new_price, Quantity new_quantity);

    // --- Read-only queries (mainly for tests / diagnostics) ------------
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Quantity quantityAt(Side side, Price price) const;
    std::size_t orderCountAt(Side side, Price price) const;
    bool contains(OrderId id) const;
    std::optional<Order> get(OrderId id) const;

    // Invariant check: the book must never be crossed, i.e. best bid must
    // be strictly less than best ask whenever both sides are non-empty.
    // Exposed publicly so tests (and, later, a monitoring hook) can assert
    // on it after every mutation.
    bool isCrossed() const;

    std::size_t bidLevelCount() const { return bids_.size(); }
    std::size_t askLevelCount() const { return asks_.size(); }

private:
    struct OrderLocation {
        Side side;
        Price price;
        std::list<Order>::iterator iterator;
    };

    // Bids sorted highest-first, asks sorted lowest-first: begin() is
    // always "best price" on both maps, which keeps matching code symmetric.
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>> asks_;

    std::unordered_map<OrderId, OrderLocation> locations_;
    Sequence next_sequence_{0};

    // Matches `incoming` against the given side's book, mutating price
    // levels as it goes and appending Fill events. Stops when either the
    // incoming order is exhausted or no more resting orders cross its price.
    template <typename BookSide>
    void matchAgainst(BookSide& opposite_book, Order& incoming, Side incoming_side,
                       std::vector<Fill>& fills);

    static bool crosses(Side incoming_side, Price incoming_price, Price resting_price);
};

} // namespace engine
