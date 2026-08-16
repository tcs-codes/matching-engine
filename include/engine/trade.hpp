#pragma once

#include "engine/types.hpp"

namespace engine {

// Emitted once per match event. An incoming order that sweeps three resting
// orders produces three Fills, each independently reportable (this maps
// cleanly onto how exchanges publish trade prints).
struct Fill {
    OrderId resting_order_id{};
    OrderId incoming_order_id{};
    Price price{};        // trades occur at the resting order's price
    Quantity quantity{};  // size of this individual match
};

enum class CancelResult {
    Cancelled,
    NotFound,
};

// Result of a cancel-replace (modify) operation.
enum class ReplaceResult {
    Replaced,
    NotFound,
};

struct CancelReplaceOutcome {
    ReplaceResult result{ReplaceResult::NotFound};
    // Whether the replace preserved the order's time priority (true only
    // when price is unchanged and quantity did not increase -- see the
    // design note on OrderBook::cancelReplace).
    bool preserved_priority{false};
    std::vector<Fill> fills; // populated if the replacement order crossed
};

} // namespace engine
