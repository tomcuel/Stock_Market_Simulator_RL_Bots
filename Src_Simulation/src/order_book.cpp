#include "order_book.hpp"

#include <algorithm>
#include <mutex>

namespace sim {

OrderBook::OrderBook(Symbol symbol) : symbol_(std::move(symbol)) {}

std::vector<Trade> OrderBook::match(Order incoming) {
    std::unique_lock lock(mutex_);
    if (incoming.side == Side::BUY) {
        return match_incoming_buy(std::move(incoming));
    }
    return match_incoming_sell(std::move(incoming));
}

std::vector<Trade> OrderBook::match_incoming_buy(Order incoming) {
    std::vector<Trade> trades;

    while (incoming.quantity > 0 && !asks_.empty()) {
        auto level_it = asks_.begin();
        Price level_price = level_it->first;

        // a LIMIT buy only crosses the spread if it is willing to pay at least the ask price
        // a MARKET buy always crosses, regardless of the resting price
        if (incoming.kind != OrderKind::MARKET && incoming.price < level_price) {
            break;
        }

        auto& queue = level_it->second;
        while (incoming.quantity > 0 && !queue.empty()) {
            Order& resting = queue.front();
            Quantity traded_qty = std::min(incoming.quantity, resting.quantity);

            Trade trade;
            trade.buy_order_id = incoming.id;
            trade.sell_order_id = resting.id;
            trade.buyer = incoming.client;
            trade.seller = resting.client;
            trade.symbol = symbol_;
            trade.quantity = traded_qty;
            trade.price = level_price; // always the resting (real, LIMIT) order's price
            trade.timestamp = Clock::now();
            trades.push_back(trade);

            incoming.quantity -= traded_qty;
            resting.quantity -= traded_qty;
            if (resting.quantity == 0) {
                queue.pop_front();
            }
        }

        if (queue.empty()) {
            asks_.erase(level_it);
        }
    }
    // LIMIT orders rest with any unfilled remainder: MARKET orders are Immediate-Or-Cancel and never rest
    if (incoming.quantity > 0 && incoming.kind != OrderKind::MARKET) {
        bids_[incoming.price].push_back(incoming);
    }

    return trades;
}

std::vector<Trade> OrderBook::match_incoming_sell(Order incoming) {
    std::vector<Trade> trades;

    while (incoming.quantity > 0 && !bids_.empty()) {
        auto level_it = bids_.begin();
        Price level_price = level_it->first;

        // a LIMIT sell only crosses the spread if it is willing to accept at most the bid price
        // a MARKET sell always crosses, regardless of the resting price
        if (incoming.kind != OrderKind::MARKET && incoming.price > level_price) {
            break;
        }

        auto& queue = level_it->second;
        while (incoming.quantity > 0 && !queue.empty()) {
            Order& resting = queue.front();
            Quantity traded_qty = std::min(incoming.quantity, resting.quantity);

            Trade trade;
            trade.buy_order_id = resting.id;
            trade.sell_order_id = incoming.id;
            trade.buyer = resting.client;
            trade.seller = incoming.client;
            trade.symbol = symbol_;
            trade.quantity = traded_qty;
            trade.price = level_price; // always the resting (real, LIMIT) order's price
            trade.timestamp = Clock::now();
            trades.push_back(trade);

            incoming.quantity -= traded_qty;
            resting.quantity -= traded_qty;
            if (resting.quantity == 0) {
                queue.pop_front();
            }
        }

        if (queue.empty()) {
            bids_.erase(level_it);
        }
    }

    if (incoming.quantity > 0 && incoming.kind != OrderKind::MARKET) {
        asks_[incoming.price].push_back(incoming);
    }

    return trades;
}

bool OrderBook::cancel(Side side, Price price, OrderId order_id) {
    std::unique_lock lock(mutex_);
    if (side == Side::BUY) {
        auto level_it = bids_.find(price);
        if (level_it == bids_.end()) {
            return false;
        }
        auto& queue = level_it->second;
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id;});
        if (it == queue.end()) {
            return false;
        }
        queue.erase(it);
        if (queue.empty()) {
            bids_.erase(level_it);
        }
        return true;
    } 
    else {
        auto level_it = asks_.find(price);
        if (level_it == asks_.end()) {
            return false;
        }
        auto& queue = level_it->second;
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id;});
        if (it == queue.end()) {
            return false;
        }
        queue.erase(it);
        if (queue.empty()) {
            asks_.erase(level_it);
        }
        return true;
    }
}

bool OrderBook::cancel(OrderId order_id) {
    std::unique_lock lock(mutex_);
    for (auto& [price, queue] : bids_) {
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id;});
        if (it != queue.end()) {
            queue.erase(it);
            if (queue.empty()) bids_.erase(price);
            return true;
        }
    }
    for (auto& [price, queue] : asks_) {
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id;});
        if (it != queue.end()) {
            queue.erase(it);
            if (queue.empty()) {
                asks_.erase(price);
            }
            return true;
        }
    }
    return false;
}

std::optional<Price> OrderBook::best_bid() const {
    std::shared_lock lock(mutex_);
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    std::shared_lock lock(mutex_);
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

BookSnapshot OrderBook::snapshot(std::size_t depth) const {
    std::shared_lock lock(mutex_);
    BookSnapshot snap;
    std::size_t count = 0;
    for (const auto& [price, queue] : bids_) {
        if (count++ >= depth) {
            break;
        }
        Quantity total = 0;
        for (const auto& o : queue) {
            total += o.quantity;
        }
        snap.bids.push_back({price, total});
    }
    count = 0;
    for (const auto& [price, queue] : asks_) {
        if (count++ >= depth) {
            break;
        }
        Quantity total = 0;
        for (const auto& o : queue) {
            total += o.quantity;
        }
        snap.asks.push_back({price, total});
    }
    return snap;
}

std::vector<Order> OrderBook::resting_orders() const {
    std::shared_lock lock(mutex_);
    std::vector<Order> result;
    for (const auto& [price, queue] : bids_) {
        result.insert(result.end(), queue.begin(), queue.end());
    }
    for (const auto& [price, queue] : asks_) {
        result.insert(result.end(), queue.begin(), queue.end());
    }
    return result;
}

} // namespace sim
