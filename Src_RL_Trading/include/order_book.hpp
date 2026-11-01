//=======================================================================
// Single-symbol order book: price-time priority (FIFO within a price level), partial fills and Immediate-Or-Cancel market orders
//Thread-safe: one shared_mutex per book, so different symbols never contend with each other
//=======================================================================
#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

#include <deque>
#include <functional>
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

    // closing auction (end-of-day fixing), see MatchingEngine::run_closing_auction
    // a trade of the auction and the price its buy order reserved per share (released when it settles)
    struct AuctionFill {
        Trade trade;
        Price buyer_reserve_price;
    };
    struct AuctionResult {
        Price price{0.0};            // the closing price, 0 if the book didn't cross
        Quantity volume{0};          // shares executed at that price
        std::vector<AuctionFill> fills;
    };
    // pre-close: the order rests WITHOUT matching, so the book may cross until the auction runs
    void add(Order order);
    // the fixing: the single price that executes the most volume (ties: the smallest unmatched quantity, then the price
    // closest to `reference`), every crossing order trades at that price by price-time priority (the Src_SQL fixing rule)
    AuctionResult uncross(Price reference);
    // end of day: removes every resting order and returns them (their reservations are released by the engine)
    std::vector<Order> remove_if(const std::function<bool(const Order&)>& which);
    std::vector<Order> clear() {return remove_if([](const Order&) {return true;});}

    // Cancels a resting order given its side and price (cheap: goes straight to the right price level): 
    // Returns the removed order (with its remaining quantity), or nothing if it wasn't found, so the engine can release exactly what that order still had reserved
    std::optional<Order> cancel(Side side, Price price, OrderId order_id, std::optional<ClientId> owner = std::nullopt);

    // Cancels a resting order by id alone, without requiring the caller to know its side/price
    // Scans both sides of the book, more expensive than the overload above, but much friendlier for a remote client that only ever learned an order id back, never its resting side/price
    // Acceptable cost here : a single symbol's book is not expected to hold more than a few thousand resting orders in this simulator (will talk later about scaling if that stops being true)
    std::optional<Order> cancel(OrderId order_id, std::optional<ClientId> owner = std::nullopt);

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

#endif // ORDER_BOOK_HPP