#include <iostream>

#include "engine/order_book.hpp"

int main() {
    using namespace engine;

    OrderBook book;

    book.addLimitOrder(1, Side::Buy, 10000, 50);   // resting bid: 100.00 x 50
    book.addLimitOrder(2, Side::Sell, 10010, 30);  // resting ask: 100.10 x 30
    auto fills = book.addLimitOrder(3, Side::Sell, 9900, 20); // crosses the bid

    std::cout << "Fills generated: " << fills.size() << "\n";
    for (const auto& f : fills) {
        std::cout << "  resting=" << f.resting_order_id
                  << " incoming=" << f.incoming_order_id
                  << " price=" << f.price
                  << " qty=" << f.quantity << "\n";
    }

    if (auto bb = book.bestBid()) std::cout << "Best bid: " << *bb << "\n";
    if (auto ba = book.bestAsk()) std::cout << "Best ask: " << *ba << "\n";

    return 0;
}
