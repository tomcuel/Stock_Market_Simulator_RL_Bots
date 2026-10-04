//=======================================================================
// A client's account: cash and share holdings, plus what open orders have reserved
//=======================================================================
#ifndef PORTFOLIO_HPP
#define PORTFOLIO_HPP

#include <algorithm>
#include <unordered_map>

#include "types.hpp"

namespace sim {

// Open orders are never debited: a client keeps owning the cash and shares an order has engaged until that order actually trades
// They are reserved instead, so the same cash (or the same shares) can't be promised to several orders at once
// Without that, several resting buys could each pass the affordability check and then all fill, leaving the client with negative cash (this happened in a real run: one bot ended at -18768.89)
//   reserved_cash      sum over open BUY orders of remaining quantity x the order's limit price (for a MARKET buy, its collar price, see MatchingEngine)
//   reserved_shares    per symbol, sum of the remaining quantity of open SELL orders
// net_worth() counts reserved cash and shares as still owned, because they are
struct Portfolio {
    double cash{0.0};
    std::unordered_map<Symbol, Quantity> holdings;
    double reserved_cash{0.0};
    std::unordered_map<Symbol, Quantity> reserved_shares;

    Quantity holding(const Symbol& symbol) const {
        auto it = holdings.find(symbol);
        return it == holdings.end() ? 0 : it->second;
    }
    
    Quantity reserved(const Symbol& symbol) const {
        auto it = reserved_shares.find(symbol);
        return it == reserved_shares.end() ? 0 : it->second;
    }

    // what a new order may still engage
    double available_cash() const {return cash - reserved_cash;}
    Quantity available_shares(const Symbol& symbol) const {return holding(symbol) - reserved(symbol);}

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

#endif // PORTFOLIO_HPP