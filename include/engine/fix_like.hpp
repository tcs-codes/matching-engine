#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/concurrent_book.hpp"
#include "engine/trade.hpp"
#include "engine/types.hpp"

namespace engine {

// ---------------------------------------------------------------------------
// A FIX-like protocol layer (Phase 6).
//
// This is a deliberate subset of FIX 4.2, enough to put real order messages
// on the wire: NewOrderSingle (35=D), OrderCancelRequest (35=F),
// OrderCancelReplaceRequest (35=G) inbound, and ExecutionReport (35=8)
// outbound. Two pragmatic deviations from full FIX, both documented:
//
//   * Fields are separated by '|' instead of SOH (\x01) so messages are
//     readable in a terminal and trivially printable -- the standard FIX
//     checksum (sum of all bytes up to and including the separator before
//     the 10= field, mod 256) is still computed and verified, so parsing is
//     exactly as strict as the real thing.
//   * Order ids: FIX uses the string ClOrdID (11); the engine uses uint64
//     ids, so we map deterministically with splitmix64(ClOrdID). Same
//     ClOrdID always maps to the same engine id.
//
// Tag subset: 8 BeginString, 9 BodyLength, 10 CheckSum, 11 ClOrdID,
// 17 ExecID, 31 LastPx, 32 LastQty, 14 CumQty, 35 MsgType, 38 OrderQty,
// 39 OrdStatus, 41 OrigClOrdID, 44 Price, 54 Side (1=Buy,2=Sell),
// 55 Symbol, 59 TimeInForce (1=GTC, 3=IOC), 150 ExecType.
// ---------------------------------------------------------------------------

inline constexpr const char* kFixBeginString = "FIX.4.2";
inline constexpr char kFixSeparator = '|';

// A parsed FIX message: the ordered raw fields. parseFixMessage returns
// nullopt for anything that fails structural or checksum validation, so an
// engaged FixMessage is always a fully valid message.
struct FixMessage {
    std::vector<std::pair<std::string, std::string>> fields;

    // First value for `tag`, or nullptr.
    const std::string* find(const std::string& tag) const;
};

// Parses and validates a raw message: 8=, 9=, 10= checksum (strict), and the
// message-type-specific required tags. Returns nullopt if invalid.
std::optional<FixMessage> parseFixMessage(std::string_view message);

// Builds a message from ordered fields (without 8/9/10 -- those are computed:
// body length from the joined body, checksum from the assembled message).
std::string makeFixMessage(std::vector<std::pair<std::string, std::string>> fields);

// Deterministic ClOrdID -> engine order id mapping (splitmix64).
OrderId orderIdFromClOrdId(const std::string& cl_ord_id);

// --- Inbound: FIX order messages -> engine ops ------------------------------
// Returns nullopt if a required field is missing or unparseable.
std::optional<OrderOp> newOrderSingleToOp(const FixMessage& m);      // 35=D
std::optional<OrderOp> orderCancelRequestToOp(const FixMessage& m);  // 35=F
std::optional<OrderOp> cancelReplaceRequestToOp(const FixMessage& m);// 35=G

// --- Outbound: execution reports --------------------------------------------
// One fill report (150=F, 39=PartiallyFilled/Filled) carrying cum/leaves.
std::string tradeExecutionReport(const FixMessage& order, const Fill& fill, Quantity cum_qty,
                                 Quantity leaves_qty, const std::string& exec_id);
// Acknowledgment (150=0 New, or 150=4 Canceled), 151=leaves, 14=cum.
std::string ackExecutionReport(const FixMessage& order, const std::string& exec_type,
                               const std::string& ord_status, Quantity leaves_qty,
                               const std::string& exec_id);

} // namespace engine
