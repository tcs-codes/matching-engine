#include "engine/order_stream.hpp"

#include <cassert>
#include <sstream>
#include <string>

namespace engine {

namespace {

Side randomSide(std::mt19937& rng) {
    return (rng() % 2 == 0) ? Side::Buy : Side::Sell;
}

} // namespace

OpStreamGenerator::OpStreamGenerator(Config cfg)
    : cfg_(cfg), rng_(cfg.seed), mid_(cfg.mid) {
    assert(cfg.min_qty > 0 && cfg.max_qty >= cfg.min_qty);
}

OrderId OpStreamGenerator::takeLiveId() {
    if (live_.empty()) return 0;
    std::uniform_int_distribution<std::size_t> pick(0, live_.size() - 1);
    const std::size_t idx = pick(rng_);
    const OrderId id = live_[idx];
    live_[idx] = live_.back();
    live_.pop_back();
    return id;
}

std::optional<OrderOp> OpStreamGenerator::next() {
    if (produced_ >= cfg_.count) {
        return std::nullopt;
    }
    ++produced_;

    // The mid price performs a slow, bounded random walk; orders cluster
    // around it. The clamp keeps generated prices inside the book's default
    // price range so the stream stays valid for any (0, 100000) book.
    if (rng_() % 20 == 0) {
        mid_ += static_cast<Price>((static_cast<int>(rng_() % 7)) - 3);
        if (mid_ < cfg_.mid - 2000) mid_ = cfg_.mid - 2000;
        if (mid_ > cfg_.mid + 2000) mid_ = cfg_.mid + 2000;
    }

    // Op mix: 0-5 add limit (60%), 6-7 cancel (20%), 8 cancel-replace (10%),
    // 9 IOC (10%). Cancels/replaces need something live to target.
    const int roll = static_cast<int>(rng_() % 10);
    if (roll >= 6 && roll <= 7 && !live_.empty()) {
        const OrderId id = takeLiveId();
        return OrderOp{OpType::Cancel, 0, id, Side::Buy, 0, 0};
    }
    if (roll == 8 && !live_.empty()) {
        // 10%: cancel-replace a live order at a fresh price/qty. The order
        // is still live afterwards (in the generator's view).
        const OrderId id = takeLiveId();
        live_.push_back(id);
        std::uniform_int_distribution<Price> price_off(-2 * cfg_.spread, 2 * cfg_.spread);
        std::uniform_int_distribution<Quantity> qty(cfg_.min_qty, cfg_.max_qty);
        return OrderOp{OpType::CancelReplace, 0, id, Side::Buy,
                       mid_ + price_off(rng_), qty(rng_)};
    }

    // Otherwise: an add (limit for rolls 0-5, IOC for roll 9).
    const bool is_ioc = (roll == 9);
    const Side side = randomSide(rng_);
    const std::uint64_t r = rng_();
    const bool aggressive_add =
        (static_cast<double>(r % 100) / 100.0) < cfg_.aggressive;

    std::uniform_int_distribution<Price> price_off(-cfg_.spread, cfg_.spread);
    std::uniform_int_distribution<Quantity> qty(cfg_.min_qty, cfg_.max_qty);
    Price price = mid_ + price_off(rng_);
    if (aggressive_add) {
        // Priced to cross a book clustered at the mid: buys pay up, sells
        // hit down. These produce fills once the opposite side has depth.
        price += side == Side::Buy ? 3 * cfg_.spread : -3 * cfg_.spread;
    }

    const OrderId id = next_id_++;
    if (!is_ioc) {
        live_.push_back(id);
    }
    return OrderOp{is_ioc ? OpType::AddIOC : OpType::AddLimit, 0, id, side, price,
                   qty(rng_)};
}

void dumpOps(std::ostream& out, const std::vector<OrderOp>& ops) {
    out << "# engine op log v1\n";
    for (const OrderOp& op : ops) {
        switch (op.type) {
            case OpType::AddLimit:
                out << "L " << op.id << ' ' << static_cast<int>(op.side) << ' ' << op.price << ' '
                    << op.quantity << '\n';
                break;
            case OpType::AddIOC:
                out << "I " << op.id << ' ' << static_cast<int>(op.side) << ' ' << op.price << ' '
                    << op.quantity << '\n';
                break;
            case OpType::Cancel:
                out << "C " << op.id << '\n';
                break;
            case OpType::CancelReplace:
                out << "R " << op.id << ' ' << op.price << ' ' << op.quantity << '\n';
                break;
            default:
                assert(false && "cannot dump non-order ops");
        }
    }
}

std::vector<OrderOp> loadOps(std::istream& in) {
    std::vector<OrderOp> ops;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        char kind = '\0';
        ls >> kind;
        OrderOp op;
        int side = 0;
        switch (kind) {
            case 'L':
            case 'I':
                ls >> op.id >> side >> op.price >> op.quantity;
                op.type = (kind == 'L') ? OpType::AddLimit : OpType::AddIOC;
                op.side = side == 0 ? Side::Buy : Side::Sell;
                break;
            case 'C':
                ls >> op.id;
                op.type = OpType::Cancel;
                break;
            case 'R':
                ls >> op.id >> op.price >> op.quantity;
                op.type = OpType::CancelReplace;
                break;
            default:
                continue; // skip unrecognized lines
        }
        ops.push_back(op);
    }
    return ops;
}

} // namespace engine
