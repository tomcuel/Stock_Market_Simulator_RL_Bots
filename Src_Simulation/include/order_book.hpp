//=======================================================================
// Single-symbol order book: price-time priority (FIFO within a price level), partial fills and Immediate-Or-Cancel market orders
//Thread-safe: one shared_mutex per book, so different symbols never contend with each other
//=======================================================================
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <shared_mutex>
#include <vector>

#include "types.hpp"

namespace sim {

// Read-only snapshot of the top of the book, used for market data / bots without holding a lock for longer than necessary
struct BookLevel {
    Price price{0.0};
    Quantity quantity{0};
};

struct BookSnapshot {
    std::vector<BookLevel> bids; // best (highest) price first
    std::vector<BookLevel> asks; // best (lowest) price first
};

class OrderBook {
public:
    explicit OrderBook(Symbol symbol);

    // Matches `incoming` against the resting book and returns the resulting trades
    // - LIMIT orders that are not fully filled rest in the book afterwards
    // - MARKET orders never rest: any unfilled remainder is discarded 
    std::vector<Trade> match(Order incoming);

    // Cancels a resting order given its side and price (cheap: goes straight to the right price level): Returns true if it was found and removed
    bool cancel(Side side, Price price, OrderId order_id);

    // Cancels a resting order by id alone, without requiring the caller to know its side/price
    bool cancel(OrderId order_id);

    // Best bid / ask, if any. Cheap read under a shared (read) lock
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Top `depth` price levels on each side, for market data / bot decision making
    BookSnapshot snapshot(std::size_t depth = 5) const;

    // Copies of every individual order currently resting in the book (both sides, best price first within each side)
    std::vector<Order> resting_orders() const;

    const Symbol& symbol() const { return symbol_; }

private:
    // MARKET orders match unconditionally against the best resting price on the other side and are Immediate-Or-Cancel, so the placeholder-price bug class cannot occur here
    // STOP/LIMIT_STOP orders never reach this class directly: they are held in an OrderRegistry until their release band is satisfied, at which point they arrive here already resolved into a resting
    // LIMIT-like order (see MatchingEngine); this class only ever needs to distinguish "MARKET" from "everything else rests at its own price", which is why the checks below use `!= OrderKind::MARKET`
    std::vector<Trade> match_incoming_buy(Order incoming);
    std::vector<Trade> match_incoming_sell(Order incoming);

    Symbol symbol_;
    mutable std::shared_mutex mutex_;
    std::map<Price, std::deque<Order>, std::greater<Price>> bids_; // highest price first
    std::map<Price, std::deque<Order>, std::less<Price>> asks_;    // lowest price first
};

} // namespace sim
