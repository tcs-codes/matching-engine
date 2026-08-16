#include "engine/order_book.hpp"

#include <cassert>

namespace engine {

bool OrderBook::crosses(Side incoming_side, Price incoming_price, Price resting_price) {
    // A buy crosses (matches) a resting sell if it's willing to pay at
    // least the sell's ask price. A sell crosses a resting buy if it's
    // willing to accept at most the buy's bid price.
    return incoming_side == Side::Buy ? incoming_price >= resting_price
                                       : incoming_price <= resting_price;
}

// Matches `incoming` against `opposite_book` (the map for the other side),
// walking price levels best-to-worst and, within each level, orders
// front-to-back (i.e. strict price-time priority). Mutates price levels in
// place, removing orders/levels that fully fill, and appends one Fill per
// match.
//
// Templated on the map type only so the same logic serves both directions
// (bids_ has a different comparator type than asks_) without duplicating
// the loop body -- there is exactly one matching algorithm, not two.
template <typename BookSide>
void OrderBook::matchAgainst(BookSide& opposite_book, Order& incoming, Side incoming_side,
                              std::vector<Fill>& fills) {
    auto level_it = opposite_book.begin();
    while (incoming.quantity > 0 && level_it != opposite_book.end()) {
        const Price level_price = level_it->first;
        if (!crosses(incoming_side, incoming.price, level_price)) {
            break; // best remaining opposite price no longer crosses -- done
        }

        PriceLevel& level = level_it->second;
        auto& orders = level.orders;

        while (incoming.quantity > 0 && !orders.empty()) {
            Order& resting = orders.front();
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
                // Resting order fully filled: remove from index and queue.
                locations_.erase(resting.id);
                orders.pop_front();
            }
            // If resting.quantity > 0, incoming must be exhausted (that's
            // the only way the inner loop condition fails), so we leave the
            // partially-filled resting order at the front of the queue --
            // it keeps its place in time priority for the next match.
        }

        if (orders.empty()) {
            level_it = opposite_book.erase(level_it);
        } else {
            break; // resting orders remain at this level => incoming is exhausted
        }
    }
}

std::vector<Fill> OrderBook::addOrder(OrderId id, Side side, Price price, Quantity quantity,
                                       OrderType type) {
    assert(quantity > 0 && "orders must have positive quantity");

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
        matchAgainst(asks_, incoming, side, fills);
    } else {
        matchAgainst(bids_, incoming, side, fills);
    }

    // IOC never rests: whatever didn't match is discarded here. This is
    // the entire difference between IOC and Limit -- matching logic above
    // is identical for both order types.
    if (incoming.quantity > 0 && type == OrderType::Limit) {
        if (side == Side::Buy) {
            PriceLevel& level = bids_[price];
            level.orders.push_back(incoming);
            level.total_quantity += incoming.quantity;
            auto it = std::prev(level.orders.end());
            locations_[id] = OrderLocation{side, price, it};
        } else {
            PriceLevel& level = asks_[price];
            level.orders.push_back(incoming);
            level.total_quantity += incoming.quantity;
            auto it = std::prev(level.orders.end());
            locations_[id] = OrderLocation{side, price, it};
        }
    }

    return fills;
}

CancelResult OrderBook::cancel(OrderId id) {
    auto loc_it = locations_.find(id);
    if (loc_it == locations_.end()) {
        return CancelResult::NotFound;
    }

    const OrderLocation loc = loc_it->second;
    locations_.erase(loc_it);

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        assert(level_it != bids_.end());
        PriceLevel& level = level_it->second;
        level.total_quantity -= loc.iterator->quantity;
        level.orders.erase(loc.iterator);
        if (level.orders.empty()) {
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(loc.price);
        assert(level_it != asks_.end());
        PriceLevel& level = level_it->second;
        level.total_quantity -= loc.iterator->quantity;
        level.orders.erase(loc.iterator);
        if (level.orders.empty()) {
            asks_.erase(level_it);
        }
    }

    return CancelResult::Cancelled;
}

CancelReplaceOutcome OrderBook::cancelReplace(OrderId id, Price new_price, Quantity new_quantity) {
    assert(new_quantity > 0 && "replace quantity must be positive; use cancel() to remove an order");

    auto loc_it = locations_.find(id);
    if (loc_it == locations_.end()) {
        return CancelReplaceOutcome{ReplaceResult::NotFound, false, {}};
    }

    const OrderLocation loc = loc_it->second;
    const Order snapshot = *loc.iterator; // copy before any mutation

    const bool preserve_priority = (new_price == snapshot.price) && (new_quantity <= snapshot.quantity);

    if (preserve_priority) {
        // Pure size reduction at the same price: adjust in place, no
        // requeue, no re-matching -- a smaller resting order can't newly
        // cross a price it was already resting at.
        const Quantity delta = snapshot.quantity - new_quantity;
        if (loc.side == Side::Buy) {
            bids_.at(loc.price).total_quantity -= delta;
        } else {
            asks_.at(loc.price).total_quantity -= delta;
        }
        loc.iterator->quantity = new_quantity;

        return CancelReplaceOutcome{ReplaceResult::Replaced, true, {}};
    }

    // Priority-losing path: remove the old order, then reinsert as if newly
    // submitted (fresh sequence number, matched against the opposite side
    // from scratch -- it may fill immediately if the new price crosses).
    cancel(id);
    std::vector<Fill> fills = addOrder(id, snapshot.side, new_price, new_quantity, OrderType::Limit);

    return CancelReplaceOutcome{ReplaceResult::Replaced, false, std::move(fills)};
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::quantityAt(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_quantity;
    } else {
        auto it = asks_.find(price);
        return it == asks_.end() ? 0 : it->second.total_quantity;
    }
}

std::size_t OrderBook::orderCountAt(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.orders.size();
    } else {
        auto it = asks_.find(price);
        return it == asks_.end() ? 0 : it->second.orders.size();
    }
}

bool OrderBook::contains(OrderId id) const {
    return locations_.contains(id);
}

std::optional<Order> OrderBook::get(OrderId id) const {
    auto it = locations_.find(id);
    if (it == locations_.end()) return std::nullopt;
    return *it->second.iterator;
}

bool OrderBook::isCrossed() const {
    auto bb = bestBid();
    auto ba = bestAsk();
    if (!bb || !ba) return false; // can't be crossed if one side is empty
    return *bb >= *ba;
}

} // namespace engine
