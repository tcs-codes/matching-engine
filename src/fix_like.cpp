#include "engine/fix_like.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace engine {

namespace {

// Sum of all bytes up to and including the last separator before the checksum
// field -- the standard FIX checksum region.
unsigned fixChecksum(std::string_view message, std::size_t up_to_exclusive) {
    unsigned sum = 0;
    for (std::size_t i = 0; i < up_to_exclusive; ++i) {
        sum += static_cast<unsigned char>(message[i]);
    }
    return sum % 256;
}

bool parseSigned(const std::string* s, std::int64_t& out) {
    if (s == nullptr || s->empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s->c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = static_cast<std::int64_t>(v);
    return true;
}

const std::string* field(const FixMessage& m, const std::string& tag) {
    return m.find(tag);
}

// Validates the message-type-specific required tags; returns false with a
// description in `error`.
bool validateRequired(const FixMessage& m, std::initializer_list<const char*> required,
                      std::string& error) {
    for (const char* tag : required) {
        if (field(m, tag) == nullptr) {
            error = std::string("missing required tag ") + tag;
            return false;
        }
    }
    return true;
}

} // namespace

const std::string* FixMessage::find(const std::string& tag) const {
    for (const auto& [t, v] : fields) {
        if (t == tag) return &v;
    }
    return nullptr;
}

std::optional<FixMessage> parseFixMessage(std::string_view message) {
    FixMessage m;

    // Split into fields on the separator.
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = message.find(kFixSeparator, start);
        if (pos == std::string_view::npos) {
            const std::string_view last = message.substr(start);
            if (!last.empty()) {
                const std::size_t eq = last.find('=');
                if (eq != std::string_view::npos) {
                    m.fields.emplace_back(std::string(last.substr(0, eq)),
                                          std::string(last.substr(eq + 1)));
                }
            }
            break;
        }
        const std::string_view f = message.substr(start, pos - start);
        if (!f.empty()) {
            const std::size_t eq = f.find('=');
            if (eq == std::string_view::npos) {
                return std::nullopt; // field without '='
            }
            m.fields.emplace_back(std::string(f.substr(0, eq)), std::string(f.substr(eq + 1)));
        }
        start = pos + 1;
    }

    // --- Structural validation: 8, 9, 10 ----------------------------------
    const std::string* bs = field(m, "8");
    if (bs == nullptr || *bs != kFixBeginString) {
        return std::nullopt; // missing or wrong BeginString
    }

    const std::string* checksum_field = field(m, "10");
    if (checksum_field == nullptr) {
        return std::nullopt; // missing CheckSum
    }

    // Checksum region: everything up to and including the '|' before 10=.
    const std::size_t checksum_pos = message.rfind("10=");
    if (checksum_pos == std::string_view::npos) {
        return std::nullopt; // malformed
    }
    const unsigned expected =
        static_cast<unsigned>(std::strtoul(checksum_field->c_str(), nullptr, 10)) % 256;
    if (fixChecksum(message, checksum_pos) != expected) {
        return std::nullopt; // checksum mismatch
    }

    const std::string* body_len_field = field(m, "9");
    if (body_len_field == nullptr) {
        return std::nullopt; // missing BodyLength
    }
    // Body = everything between the 9= value and the 10= field, i.e. the
    // joined fields in between plus their separators. Reconstruct exactly:
    // body length = position of "10=" minus position just after "9=<len>|".
    const std::size_t body_len_start = message.find(kFixSeparator, message.find("9=")) + 1;
    const std::int64_t declared = std::strtoll(body_len_field->c_str(), nullptr, 10);
    const std::int64_t actual = static_cast<std::int64_t>(checksum_pos - body_len_start);
    if (declared != actual) {
        return std::nullopt; // BodyLength mismatch
    }

    // --- Message-type-specific required tags -------------------------------
    const std::string* type = field(m, "35");
    if (type == nullptr) {
        return std::nullopt; // missing MsgType
    }
    std::string err;
    if (*type == "D") {
        if (!validateRequired(m, {"11", "55", "54", "44", "38"}, err)) {
            return std::nullopt;
        }
    } else if (*type == "F") {
        if (!validateRequired(m, {"41"}, err)) {
            return std::nullopt;
        }
    } else if (*type == "G") {
        if (!validateRequired(m, {"41", "44", "38"}, err)) {
            return std::nullopt;
        }
    }
    // "8" (execution report) and others: accept structurally valid messages;
    // converters below only handle the order messages.

    return m;
}

std::string makeFixMessage(std::vector<std::pair<std::string, std::string>> fields) {
    std::string body;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) body += kFixSeparator;
        body += fields[i].first + "=" + fields[i].second;
    }
    // Per the FIX spec, BodyLength counts the characters after "9=<len>|"
    // up to AND including the separator before the CheckSum field -- so the
    // trailing separator is part of the counted body.
    std::string msg = std::string("8=") + kFixBeginString + kFixSeparator + "9=" +
                      std::to_string(body.size() + 1) + kFixSeparator + body + kFixSeparator;
    const unsigned sum = fixChecksum(msg, msg.size());
    char buf[8];
    std::snprintf(buf, sizeof buf, "%03u", sum);
    msg += "10=" + std::string(buf) + kFixSeparator;
    return msg;
}

OrderId orderIdFromClOrdId(const std::string& cl_ord_id) {
    std::uint64_t h = 0xcbf29ce484222325ull; // FNV offset basis, then splitmix
    for (unsigned char c : cl_ord_id) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return static_cast<OrderId>(mix64(h));
}

std::optional<OrderOp> newOrderSingleToOp(const FixMessage& m) {
    const std::string* cl = field(m, "11");
    const std::string* side = field(m, "54");
    const std::string* price = field(m, "44");
    const std::string* qty = field(m, "38");
    const std::string* tif = field(m, "59");
    if (cl == nullptr || side == nullptr || price == nullptr || qty == nullptr) {
        return std::nullopt;
    }
    std::int64_t p = 0, q = 0;
    if (!parseSigned(price, p) || !parseSigned(qty, q) || p <= 0 || q <= 0) {
        return std::nullopt;
    }
    const Side s = (*side == "2") ? Side::Sell : Side::Buy;
    const OrderType type = (tif != nullptr && *tif == "3") ? OrderType::IOC : OrderType::Limit;
    return OrderOp{type == OrderType::IOC ? OpType::AddIOC : OpType::AddLimit, 0,
                   orderIdFromClOrdId(*cl), s, static_cast<Price>(p), static_cast<Quantity>(q)};
}

std::optional<OrderOp> orderCancelRequestToOp(const FixMessage& m) {
    const std::string* orig = field(m, "41");
    if (orig == nullptr) return std::nullopt;
    return OrderOp{OpType::Cancel, 0, orderIdFromClOrdId(*orig), Side::Buy, 0, 0};
}

std::optional<OrderOp> cancelReplaceRequestToOp(const FixMessage& m) {
    const std::string* orig = field(m, "41");
    const std::string* price = field(m, "44");
    const std::string* qty = field(m, "38");
    if (orig == nullptr || price == nullptr || qty == nullptr) return std::nullopt;
    std::int64_t p = 0, q = 0;
    if (!parseSigned(price, p) || !parseSigned(qty, q) || p <= 0 || q <= 0) {
        return std::nullopt;
    }
    return OrderOp{OpType::CancelReplace, 0, orderIdFromClOrdId(*orig), Side::Buy,
                   static_cast<Price>(p), static_cast<Quantity>(q)};
}

std::string tradeExecutionReport(const FixMessage& order, const Fill& fill, Quantity cum_qty,
                                 Quantity leaves_qty, const std::string& exec_id) {
    const std::string* cl = field(order, "11");
    const std::string* side = field(order, "54");
    const std::string* symbol = field(order, "55");
    const std::string* price = field(order, "44");
    const std::string* qty = field(order, "38");
    const std::string status = leaves_qty == 0 ? "2" : "1"; // Filled / PartiallyFilled
    return makeFixMessage({
        {"35", "8"},
        {"11", cl != nullptr ? *cl : ""},
        {"17", exec_id},
        {"150", "F"},
        {"39", status},
        {"55", symbol != nullptr ? *symbol : ""},
        {"54", side != nullptr ? *side : ""},
        {"44", price != nullptr ? *price : ""},
        {"38", qty != nullptr ? *qty : ""},
        {"32", std::to_string(fill.quantity)},
        {"31", std::to_string(fill.price)},
        {"151", std::to_string(leaves_qty)},
        {"14", std::to_string(cum_qty)},
    });
}

std::string ackExecutionReport(const FixMessage& order, const std::string& exec_type,
                               const std::string& ord_status, Quantity leaves_qty,
                               const std::string& exec_id) {
    const std::string* cl = field(order, "11");
    const std::string* side = field(order, "54");
    const std::string* symbol = field(order, "55");
    const std::string* price = field(order, "44");
    const std::string* qty = field(order, "38");
    return makeFixMessage({
        {"35", "8"},
        {"11", cl != nullptr ? *cl : ""},
        {"17", exec_id},
        {"150", exec_type},
        {"39", ord_status},
        {"55", symbol != nullptr ? *symbol : ""},
        {"54", side != nullptr ? *side : ""},
        {"44", price != nullptr ? *price : ""},
        {"38", qty != nullptr ? *qty : ""},
        {"151", std::to_string(leaves_qty)},
        {"14", "0"},
    });
}

} // namespace engine
