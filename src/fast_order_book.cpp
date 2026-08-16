#include "engine/fast_order_book.hpp"

#include <algorithm>
#include <cassert>

namespace engine {

namespace {

void setLevelBit(std::vector<std::uint64_t>& bits, std::size_t idx) {
    bits[idx >> 6] |= (std::uint64_t{1} << (idx & 63));
}

void clearLevelBit(std::vector<std::uint64_t>& bits, std::size_t idx) {
    bits[idx >> 6] &= ~(std::uint64_t{1} << (idx & 63));
}

} // namespace

FastOrderBook::FastOrderBook(Price min_price, Price max_price, std::size_t order_capacity)
    : min_price_(min_price),
      max_price_(max_price),
      range_(static_cast<std::size_t>(max_price - min_price + 1)),
      bids_(range_),
      asks_(range_),
      bid_bits_((range_ + 63) / 64, 0),
      ask_bits_((range_ + 63) / 64, 0),
      pool_(order_capacity) {
    assert(max_price >= min_price && "price range must be non-empty");
    // Reserve a *modest* bucket count: reserving to arena capacity (up to a
    // million orders) would put every hash lookup on a cold cache line even
    // when the book holds a handful of orders. Amortized rehashing as the
    // map grows is cheap and keeps the table hot.
    locations_.reserve(std::min<std::size_t>(order_capacity, 1u << 16));
}

bool FastOrderBook::crosses(Side incoming_side, Price incoming_price, Price resting_price) {
    // A buy crosses (matches) a resting sell if it's willing to pay at
    // least the sell's ask price. A sell crosses a resting buy if it's
    // willing to accept at most the buy's bid price. Same rule as Phase 1.
    return incoming_side == Side::Buy ? incoming_price >= resting_price
                                       : incoming_price <= resting_price;
}

std::size_t FastOrderBook::idxOf(Price price) const {
    return static_cast<std::size_t>(price - min_price_);
}

bool FastOrderBook::inRange(Price price) const {
    return price >= min_price_ && price <= max_price_;
}

void FastOrderBook::pushBack(PriceLevel& level, int32_t node_idx) {
    OrderNode& node = pool_[node_idx];
    node.prev = level.tail;
    node.next = -1;
    if (level.tail != -1) {
        pool_[level.tail].next = node_idx;
    } else {
        level.head = node_idx;
    }
    level.tail = node_idx;
    ++level.order_count;
}

void FastOrderBook::unlink(PriceLevel& level, int32_t node_idx) {
    OrderNode& node = pool_[node_idx];
    const int32_t prev = node.prev;
    const int32_t next = node.next;
    if (prev != -1) {
        pool_[prev].next = next;
    } else {
        level.head = next;
    }
    if (next != -1) {
        pool_[next].prev = prev;
    } else {
        level.tail = prev;
    }
    node.prev = -1;
    node.next = -1;
    --level.order_count;
}

int64_t FastOrderBook::nextNonEmpty(const std::vector<std::uint64_t>& bits, int64_t idx,
                                    int64_t step) const {
    // Find the first non-empty level at/after idx+step using the per-side
    // bitset. Empty levels are skipped 64 at a time (one word per check)
    // instead of one PriceLevel struct at a time, and a found bit resolves
    // to an exact index with a single ctz/clz-style intrinsic.
    if (idx == -1) return -1;

    if (step > 0) {
        const int64_t pos = idx + 1;
        if (pos >= static_cast<int64_t>(range_)) return -1;
        int64_t word = pos >> 6;
        std::uint64_t w = bits[static_cast<std::size_t>(word)] & (~std::uint64_t{0} << (pos & 63));
        for (;;) {
            if (w != 0) {
                return (word << 6) + static_cast<int64_t>(std::countr_zero(w));
            }
            ++word;
            if (word >= static_cast<int64_t>(bits.size())) return -1;
            w = bits[static_cast<std::size_t>(word)];
        }
    } else {
        const int64_t pos = idx - 1;
        if (pos < 0) return -1;
        int64_t word = pos >> 6;
        const int bit = static_cast<int>(pos & 63);
        const std::uint64_t mask =
            bit == 63 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (bit + 1)) - 1);
        std::uint64_t w = bits[static_cast<std::size_t>(word)] & mask;
        for (;;) {
            if (w != 0) {
                return (word << 6) + static_cast<int64_t>(std::bit_width(w)) - 1;
            }
            --word;
            if (word < 0) return -1;
            w = bits[static_cast<std::size_t>(word)];
        }
    }
}

// Matches `incoming` against `opposite` (the level array of the other side),
// walking price levels best-to-worst and, within each level, orders
// front-to-back (strict price-time priority). `step` is +1 when walking asks
// upward from the best ask, -1 when walking bids downward from the best bid.
// `best_idx` is the side's best-level cursor and is advanced as levels are
// consumed, so it always ends pointing at the new best (or -1). `level_count`
// tracks how many non-empty levels remain on that side.
//
// Behavior is identical to Phase 1/2 OrderBook::matchAgainst -- this is the
// same algorithm over a different container.
void FastOrderBook::matchAgainst(std::vector<PriceLevel>& opposite, Order& incoming,
                                 Side incoming_side, std::vector<Fill>& fills, int64_t& best_idx,
                                 int64_t step, std::size_t& level_count,
                                 std::vector<std::uint64_t>& bits) {
    int64_t idx = best_idx;
    while (incoming.quantity > 0 && idx != -1) {
        const Price level_price = min_price_ + static_cast<Price>(idx);
        if (!crosses(incoming_side, incoming.price, level_price)) [[unlikely]] {
            break; // best remaining opposite price no longer crosses -- done
        }

        PriceLevel& level = opposite[static_cast<std::size_t>(idx)];
        while (incoming.quantity > 0 && level.head != -1) {
            OrderNode& resting = pool_[level.head];
            const Quantity fill_qty = std::min(incoming.quantity, resting.quantity);

            fills.push_back(Fill{
                .resting_order_id = resting.id,
                .incoming_order_id = incoming.id,
                .price = resting.price,
                .quantity = fill_qty,
            });

            incoming.quantity -= fill_qty;
            resting.quantity -= fill_qty;
            level.total_quantity -= fill_qty;

            if (resting.quantity == 0) {
                // Resting order fully filled: remove from index, unlink from
                // the FIFO queue, and return its node to the pool.
                const int32_t consumed = level.head;
                locations_.erase(resting.id);
                unlink(level, consumed);
                pool_.release(consumed);
            }
            // If resting.quantity > 0, incoming must be exhausted (that's the
            // only way the inner loop condition fails), so the partially-filled
            // order stays at the front of the queue -- it keeps its place in
            // time priority for the next match.
        }

        if (level.head == -1) {
            // This level went fully non-empty -> empty: drop the level count,
            // clear its bit, and advance the cursor to the next non-empty
            // level (or -1).
            --level_count;
            clearLevelBit(bits, static_cast<std::size_t>(idx));
            idx = nextNonEmpty(bits, idx, step);
        } else {
            break; // resting orders remain at this level => incoming is exhausted
        }
    }
    best_idx = idx;
}

std::vector<Fill> FastOrderBook::addOrder(OrderId id, Side side, Price price, Quantity quantity,
                                          OrderType type) {
    assert(quantity > 0 && "orders must have positive quantity");
    assert(inRange(price) && "price outside the book's configured range");

    Order incoming{
        .id = id,
        .side = side,
        .price = price,
        .quantity = quantity,
        .original_quantity = quantity,
        .sequence = next_sequence_++,
    };

    std::vector<Fill> fills;
    if (side == Side::Buy) {
        matchAgainst(asks_, incoming, side, fills, best_ask_idx_, +1, ask_level_count_, ask_bits_);
    } else {
        matchAgainst(bids_, incoming, side, fills, best_bid_idx_, -1, bid_level_count_, bid_bits_);
    }

    // IOC never rests: whatever didn't match is discarded here. This is the
    // entire difference between IOC and Limit -- identical to Phase 2.
    if (incoming.quantity > 0 && type == OrderType::Limit) [[likely]] {
        rest(incoming);
    }

    return fills;
}

void FastOrderBook::rest(const Order& incoming) {
    const std::size_t i = idxOf(incoming.price);
    PriceLevel& level = (incoming.side == Side::Buy ? bids_ : asks_)[i];

    const int32_t node_idx = pool_.alloc();
    OrderNode& node = pool_[node_idx];
    node.id = incoming.id;
    node.side = incoming.side;
    node.price = incoming.price;
    node.quantity = incoming.quantity;
    node.original_quantity = incoming.original_quantity;
    node.sequence = incoming.sequence;

    pushBack(level, node_idx);
    level.total_quantity += incoming.quantity;
    locations_[incoming.id] = node_idx;

    if (level.order_count == 1) {
        // First order at this price level -- it may be a new best price.
        if (incoming.side == Side::Buy) {
            ++bid_level_count_;
            setLevelBit(bid_bits_, i);
            if (best_bid_idx_ == -1 || static_cast<int64_t>(i) > best_bid_idx_) {
                best_bid_idx_ = static_cast<int64_t>(i); // bids: higher price = better
            }
        } else {
            ++ask_level_count_;
            setLevelBit(ask_bits_, i);
            if (best_ask_idx_ == -1 || static_cast<int64_t>(i) < best_ask_idx_) {
                best_ask_idx_ = static_cast<int64_t>(i); // asks: lower price = better
            }
        }
    }
}

CancelResult FastOrderBook::cancel(OrderId id) {
    const int32_t* loc = locations_.find(id);
    if (loc == nullptr) [[unlikely]] {
        return CancelResult::NotFound;
    }

    const int32_t node_idx = *loc;
    locations_.erase(id);

    OrderNode& node = pool_[node_idx];
    const Side side = node.side;
    const Price price = node.price;
    const std::size_t i = idxOf(price);

    std::vector<PriceLevel>& levels = side == Side::Buy ? bids_ : asks_;
    PriceLevel& level = levels[i];
    level.total_quantity -= node.quantity;
    unlink(level, node_idx);
    pool_.release(node_idx);

    if (level.order_count == 0) {
        if (side == Side::Buy) {
            --bid_level_count_;
            clearLevelBit(bid_bits_, i);
            if (static_cast<int64_t>(i) == best_bid_idx_) {
                best_bid_idx_ = nextNonEmpty(bid_bits_, static_cast<int64_t>(i), -1);
            }
        } else {
            --ask_level_count_;
            clearLevelBit(ask_bits_, i);
            if (static_cast<int64_t>(i) == best_ask_idx_) {
                best_ask_idx_ = nextNonEmpty(ask_bits_, static_cast<int64_t>(i), +1);
            }
        }
    }

    return CancelResult::Cancelled;
}

CancelReplaceOutcome FastOrderBook::cancelReplace(OrderId id, Price new_price,
                                                  Quantity new_quantity) {
    assert(new_quantity > 0 &&
           "replace quantity must be positive; use cancel() to remove an order");
    assert(inRange(new_price) && "new price outside the book's configured range");

    const int32_t* node = locations_.find(id);
    if (node == nullptr) [[unlikely]] {
        return CancelReplaceOutcome{ReplaceResult::NotFound, false, {}};
    }

    const int32_t node_idx = *node;
    const OrderNode snapshot = pool_[node_idx]; // copy before any mutation

    const bool preserve_priority =
        (new_price == snapshot.price) && (new_quantity <= snapshot.quantity);

    if (preserve_priority) {
        // Pure size reduction at the same price: adjust in place, no requeue,
        // no re-matching -- a smaller resting order can't newly cross a price
        // it was already resting at. Same rule as Phase 2.
        OrderNode& node = pool_[node_idx];
        const Quantity delta = node.quantity - new_quantity;
        std::vector<PriceLevel>& levels = node.side == Side::Buy ? bids_ : asks_;
        levels[idxOf(node.price)].total_quantity -= delta;
        node.quantity = new_quantity;
        return CancelReplaceOutcome{ReplaceResult::Replaced, true, {}};
    }

    // Priority-losing path: remove the old order, then reinsert as if newly
    // submitted (fresh sequence number, matched against the opposite side
    // from scratch -- it may fill immediately if the new price crosses).
    cancel(id);
    std::vector<Fill> fills = addOrder(id, snapshot.side, new_price, new_quantity, OrderType::Limit);
    return CancelReplaceOutcome{ReplaceResult::Replaced, false, std::move(fills)};
}

std::optional<Price> FastOrderBook::bestBid() const {
    if (best_bid_idx_ == -1) return std::nullopt;
    return min_price_ + static_cast<Price>(best_bid_idx_);
}

std::optional<Price> FastOrderBook::bestAsk() const {
    if (best_ask_idx_ == -1) return std::nullopt;
    return min_price_ + static_cast<Price>(best_ask_idx_);
}

Quantity FastOrderBook::quantityAt(Side side, Price price) const {
    if (!inRange(price)) return 0;
    return (side == Side::Buy ? bids_ : asks_)[idxOf(price)].total_quantity;
}

std::size_t FastOrderBook::orderCountAt(Side side, Price price) const {
    if (!inRange(price)) return 0;
    return (side == Side::Buy ? bids_ : asks_)[idxOf(price)].order_count;
}

bool FastOrderBook::contains(OrderId id) const {
    return locations_.contains(id);
}

std::optional<Order> FastOrderBook::get(OrderId id) const {
    const int32_t* node = locations_.find(id);
    if (node == nullptr) return std::nullopt;
    const OrderNode& n = pool_[*node];
    return Order{n.id, n.side, n.price, n.quantity, n.original_quantity, n.sequence};
}

bool FastOrderBook::isCrossed() const {
    auto bb = bestBid();
    auto ba = bestAsk();
    if (!bb || !ba) return false; // can't be crossed if one side is empty
    return *bb >= *ba;
}

} // namespace engine
