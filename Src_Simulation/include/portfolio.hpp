#pragma once

#include <unordered_map>

#include "types.hpp"

namespace sim {

struct Portfolio {
    double cash{0.0};
    std::unordered_map<Symbol, Quantity> holdings;

    Quantity holding(const Symbol& symbol) const {
        auto it = holdings.find(symbol);
        return it == holdings.end() ? 0 : it->second;
    }

    // net worth given a map of last-traded prices (missing symbols are valued at 0)
    double net_worth(const std::unordered_map<Symbol, Price>& last_prices) const {
        double total = cash;
        for (const auto& [symbol, qty] : holdings) {
            auto it = last_prices.find(symbol);
            if (it != last_prices.end()) {
                total += static_cast<double>(qty) * it->second;
            }
        }
        return total;
    }
};

} // namespace sim
