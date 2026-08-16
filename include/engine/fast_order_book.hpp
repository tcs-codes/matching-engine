#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/flat_hash_map.hpp"
#include "engine/order.hpp"
#include "engine/trade.hpp"
#include "engine/types.hpp"

namespace engine {

// ---------------------------------------------------------------------------
// OrderNode -- the intrusive-list node that lives in the pool.
//
// Phase 1 stored orders in a std::list<Order> and pointed at them from a
// hash index with std::list iterators. Phase 3 embeds the links directly in
// the node: `prev`/`next` are *pool indices*, not pointers, so the whole
// order pool can live in one contiguous block (cache-friendly) and nodes can
// be freed and reused without any heap traffic. The tradeoff vs. Phase 1:
// an order's address is no longer stable, so callers must never hold a
// reference to a node across another mutation -- the public API only ever
// returns Order *values*, which sidesteps this entirely.
// ---------------------------------------------------------------------------
struct OrderNode {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity quantity{};          // remaining, live quantity -- shrinks on partial fill
    Quantity original_quantity{}; // quantity at insertion time, for reporting/tests
    Sequence sequence{};          // insertion order, used for time priority
    int32_t prev{-1};             // pool index of previous node in the level's FIFO queue, -1 if none
    int32_t next{-1};             // pool index of next node in the level's FIFO queue, -1 if none
};

// ---------------------------------------------------------------------------
// OrderPool -- fixed-capacity arena for OrderNode.
//
// Pre-allocates `capacity` nodes contiguously at construction. The hot path
// never allocates: alloc() pops a free-list slot (or constructs one inside
// the already-reserved block), release() pushes the slot back. Zero heap
// allocation after construction, ever. The price for the guarantees is a
// hard cap: once every slot is live, alloc() throws.
// ---------------------------------------------------------------------------
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity) : capacity_(capacity) {
        nodes_.reserve(capacity);
    }

    int32_t alloc() {
        if (!free_list_.empty()) [[likely]] { // steady state: slots are recycled
            const int32_t idx = free_list_.back();
            free_list_.pop_back();
            nodes_[static_cast<std::size_t>(idx)] = OrderNode{}; // reset to pristine
            return idx;
        }
        if (nodes_.size() >= capacity_) {
            throw std::runtime_error("order pool exhausted (capacity " + std::to_string(capacity_) +
                                     ")");
        }
        nodes_.emplace_back();
        return static_cast<int32_t>(nodes_.size()) - 1;
    }

    void release(int32_t idx) { free_list_.push_back(idx); }

    OrderNode& operator[](int32_t idx) { return nodes_[static_cast<std::size_t>(idx)]; }
    const OrderNode& operator[](int32_t idx) const {
        return nodes_[static_cast<std::size_t>(idx)];
    }

private:
    std::vector<OrderNode> nodes_;
    std::vector<int32_t> free_list_;
    std::size_t capacity_;
};

// ---------------------------------------------------------------------------
// FastOrderBook -- Phase 3: the same matching semantics as OrderBook, with
// data structures chosen for speed instead of clarity. It is NOT a new
// matching algorithm: price-time priority, partial fills, IOC, cancel-replace
// priority rules, and Fill event shape are all identical to Phase 1/2, and
// the differential fuzz test (tests/test_differential.cpp) feeds the same
// op stream to both books and asserts byte-for-byte agreement after every
// single operation.
//
// WHAT CHANGED vs. OrderBook (Phase 1/2)
// --------------------------------------
// * std::map<Price, PriceLevel>  ->  flat std::vector<PriceLevel> indexed by
//   (price - min_price). Prices are integer ticks in a bounded range (chosen
//   at construction), so "find the price level" is O(1) array indexing and
//   "advance to the next level" is pointer-free integer arithmetic. The best
//   price is tracked with a cursor per side, so bestBid()/bestAsk() are O(1)
//   and matching walks levels without any tree traversal. Advancing the
//   cursor past an emptied level uses a per-side *bitset of non-empty
//   levels* (one bit per price level) instead of scanning every level
//   struct: a worst-case "find the next non-empty level" walk touches
//   range/64 words instead of range levels, and in practice it's one
//   __builtin_ctz-style bit lookup.
//
// * std::unordered_map<OrderId, std::list<Order>::iterator>  ->  Phase 6: a
//   custom open-addressing FlatHashMap over int32 pool indices. Same O(1)
//   average lookup, but one contiguous table (no per-node allocation, no
//   pointer chasing), reserved once at construction. Erase uses backward-
//   shift deletion so a cancel-heavy book never degrades probe chains.
//   Mixed with a splitmix64 finalizer so sequential/patterned ids spread
//   evenly across power-of-two slots.
// * std::list<Order>             ->  intrusive doubly-linked list of pool
//   indices inside the PriceLevel (head/tail), backed by the arena OrderPool.
//   No per-order heap allocation, contiguous storage, and cancel is still
//   O(1): the location index points straight at the node.
//
// WHAT DOESN'T CHANGE
// -------------------
// The public API is identical to OrderBook (addLimitOrder / addIOCOrder /
// addOrder / cancel / cancelReplace / bestBid / bestAsk / quantityAt /
// orderCountAt / contains / get / isCrossed / level counts), so Phase 1/2
// call sites and tests keep working unchanged.
//
// PRICE RANGE
// -----------
// Prices outside [min_price, max_price] are a programming error: addOrder /
// cancelReplace assert in debug builds (documented precondition). Read-only
// queries return "nothing" (0 / 0 orders) for out-of-range prices instead,
// so diagnostics never crash on a stray tick.
// ---------------------------------------------------------------------------
class FastOrderBook {
public:
    // `min_price`/`max_price` bound the price range (ticks, inclusive) that
    // this book can hold; `order_capacity` is the arena size in resting
    // orders. Both are chosen at construction so the hot path has zero
    // allocation and no range checks to argue about. Defaults suit the demo,
    // tests, and benchmarks; a production deployment would size these to the
    // expected instrument's price grid and peak queue depth.
    explicit FastOrderBook(Price min_price = 0, Price max_price = 100000,
                           std::size_t order_capacity = 1u << 20);

    std::vector<Fill> addOrder(OrderId id, Side side, Price price, Quantity quantity,
                               OrderType type = OrderType::Limit);

    std::vector<Fill> addLimitOrder(OrderId id, Side side, Price price, Quantity quantity) {
        return addOrder(id, side, price, quantity, OrderType::Limit);
    }

    std::vector<Fill> addIOCOrder(OrderId id, Side side, Price price, Quantity quantity) {
        return addOrder(id, side, price, quantity, OrderType::IOC);
    }

    CancelResult cancel(OrderId id);
    CancelReplaceOutcome cancelReplace(OrderId id, Price new_price, Quantity new_quantity);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Quantity quantityAt(Side side, Price price) const;
    std::size_t orderCountAt(Side side, Price price) const;
    bool contains(OrderId id) const;
    std::optional<Order> get(OrderId id) const;
    bool isCrossed() const;

    std::size_t bidLevelCount() const { return bid_level_count_; }
    std::size_t askLevelCount() const { return ask_level_count_; }

private:
    struct PriceLevel {
        int32_t head{-1};          // pool index of front of FIFO queue, -1 if empty
        int32_t tail{-1};          // pool index of back of FIFO queue, -1 if empty
        Quantity total_quantity{0};
        std::size_t order_count{0};
    };

    Price min_price_;
    Price max_price_;
    std::size_t range_; // max_price_ - min_price_ + 1

    // One PriceLevel per tick, indexed by (price - min_price_). Bids and asks
    // each get their own array (same index space, different direction of
    // "best": bids best = highest index, asks best = lowest index).
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;

    // One bit per price level, set iff that level is non-empty. Used by
    // nextNonEmpty to skip empty levels 64 at a time instead of scanning
    // each PriceLevel struct.
    std::vector<std::uint64_t> bid_bits_;
    std::vector<std::uint64_t> ask_bits_;

    // O(1) cancel-by-id: OrderId -> pool index of its node. Open-addressing
    // table (see flat_hash_map.hpp); IdHash applies a splitmix finalizer.
    FlatHashMap<OrderId, int32_t, IdHash> locations_;
    OrderPool pool_;

    Sequence next_sequence_{0};

    // Cursor to the current best (highest non-empty bid, lowest non-empty
    // ask) price level index, or -1 if that side is empty. Advanced via the
    // per-side bitset, so a single "find next" walk is a handful of words.
    int64_t best_bid_idx_{-1};
    int64_t best_ask_idx_{-1};
    std::size_t bid_level_count_{0};
    std::size_t ask_level_count_{0};

    std::size_t idxOf(Price price) const;
    bool inRange(Price price) const;

    void pushBack(PriceLevel& level, int32_t node_idx);
    void unlink(PriceLevel& level, int32_t node_idx);

    // First non-empty level at/after idx+step, or -1 if none. Uses the
    // per-side bitset, so the walk skips 64 empty levels per word.
    int64_t nextNonEmpty(const std::vector<std::uint64_t>& bits, int64_t idx, int64_t step) const;

    void matchAgainst(std::vector<PriceLevel>& opposite, Order& incoming, Side incoming_side,
                      std::vector<Fill>& fills, int64_t& best_idx, int64_t step,
                      std::size_t& level_count, std::vector<std::uint64_t>& bits);
    void rest(const Order& incoming);

    static bool crosses(Side incoming_side, Price incoming_price, Price resting_price);
};

} // namespace engine
