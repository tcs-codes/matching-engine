#pragma once

#include "engine/types.hpp"

namespace engine {

// A single resting (or in-flight) order.
//
// Deliberately a plain struct, not a class with invariants hidden behind
// accessors: at this stage the OrderBook owns all mutation logic, and an
// aggregate is trivially copyable/movable, which keeps the std::deque
// operations in PriceLevel cheap and simple to reason about. Phase 3 will
// likely turn this into an intrusive-list node (see order_book.hpp notes),
// at which point identity and pointer stability start to matter.
struct Order {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity quantity{};       // remaining, live quantity -- shrinks on partial fill
    Quantity original_quantity{}; // quantity at insertion time, for reporting/tests
    Sequence sequence{};       // insertion order, used for time priority
};

} // namespace engine
