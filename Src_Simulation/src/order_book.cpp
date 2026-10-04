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
        if (incoming.price > 0.0 && incoming.price < level_price) {
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
        if (incoming.price > 0.0 && incoming.price > level_price) {
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

std::optional<Order> OrderBook::cancel(Side side, Price price, OrderId order_id, std::optional<ClientId> owner) {
    std::unique_lock lock(mutex_);
    if (side == Side::BUY) {
        auto level_it = bids_.find(price);
        if (level_it == bids_.end()) {
            return std::nullopt;
        }
        auto& queue = level_it->second;
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id && (!owner || o.client == *owner);});
        if (it == queue.end()) {
            return std::nullopt;
        }
        Order removed = *it;
        queue.erase(it);
        if (queue.empty()) {
            bids_.erase(level_it);
        }
        return removed;
    } 
    else {
        auto level_it = asks_.find(price);
        if (level_it == asks_.end()) {
            return std::nullopt;
        }
        auto& queue = level_it->second;
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id && (!owner || o.client == *owner);});
        if (it == queue.end()) {
            return std::nullopt;
        }
        Order removed = *it;
        queue.erase(it);
        if (queue.empty()) {
            asks_.erase(level_it);
        }
        return removed;
    }
}

std::optional<Order> OrderBook::cancel(OrderId order_id, std::optional<ClientId> owner) {
    std::unique_lock lock(mutex_);
    for (auto& [price, queue] : bids_) {
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id && (!owner || o.client == *owner);});
        if (it != queue.end()) {
            Order removed = *it;
            queue.erase(it);
            if (queue.empty()) bids_.erase(price);
            return removed;
        }
    }
    for (auto& [price, queue] : asks_) {
        auto it = std::find_if(queue.begin(), queue.end(), [&](const Order& o) {return o.id == order_id && (!owner || o.client == *owner);});
        if (it != queue.end()) {
            Order removed = *it;
            queue.erase(it);
            if (queue.empty()) {
                asks_.erase(price);
            }
            return removed;
        }
    }
    return std::nullopt;
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

// closing auction
void OrderBook::add(Order order) {
    std::unique_lock lock(mutex_);
    if (order.side == Side::BUY) {
        bids_[order.price].push_back(std::move(order));
    } 
    else {
        asks_[order.price].push_back(std::move(order));
    }
}

OrderBook::AuctionResult OrderBook::uncross(Price reference) {
    std::unique_lock lock(mutex_);
    AuctionResult result;
    if (bids_.empty() || asks_.empty() || bids_.begin()->first < asks_.begin()->first) {
        return result; // nothing crosses: no auction trade, the close stays the last traded price
    }
    // every price level of both sides is a candidate closing price
    std::vector<Price> candidates;
    for (const auto& [price, queue] : bids_) {
        candidates.push_back(price);
    }
    for (const auto& [price, queue] : asks_) {
        candidates.push_back(price);
    }
    auto side_volume = [](const std::deque<Order>& queue) {
        Quantity total = 0;
        for (const auto& o : queue) {
            total += o.quantity;
        }
        return total;
    };
    Quantity best_volume = 0, best_imbalance = 0;
    Price best_price = 0.0;
    for (Price p : candidates) {
        Quantity demand = 0, supply = 0; // buyers willing to pay p or more, sellers willing to sell at p or less
        for (const auto& [price, queue] : bids_) {
            if (price < p) {
                break;
            }
            demand += side_volume(queue);
        }
        for (const auto& [price, queue] : asks_) {
            if (price > p) {
                break;
            }
            supply += side_volume(queue);
        }
        Quantity volume = std::min(demand, supply);
        Quantity imbalance = demand > supply ? demand - supply : supply - demand;
        bool better = volume > best_volume || (volume == best_volume && volume > 0 && imbalance < best_imbalance) || (volume == best_volume && volume > 0 && imbalance == best_imbalance && std::abs(p - reference) < std::abs(best_price - reference));
        if (better) {
            best_volume = volume;
            best_imbalance = imbalance;
            best_price = p;
        }
    }
    if (best_volume == 0) {
        return result;
    }
    result.price = best_price;
    result.volume = best_volume;

    // execute best_volume at best_price: best bids against best asks, oldest first inside a level
    Quantity remaining = best_volume;
    TimePoint now = Clock::now();
    while (remaining > 0 && !bids_.empty() && !asks_.empty()) {
        auto bid_level = bids_.begin();
        auto ask_level = asks_.begin();
        if (bid_level->first < best_price || ask_level->first > best_price) {
            break;
        }
        Order& buy = bid_level->second.front();
        Order& sell = ask_level->second.front();
        Quantity quantity = std::min({buy.quantity, sell.quantity, remaining});
        Trade trade;
        trade.buy_order_id = buy.id;
        trade.sell_order_id = sell.id;
        trade.buyer = buy.client;
        trade.seller = sell.client;
        trade.symbol = buy.symbol;
        trade.quantity = quantity;
        trade.price = best_price; // everyone trades at the single closing price
        trade.timestamp = now;
        result.fills.push_back(AuctionFill{trade, buy.price}); // the buy order reserved its own price per share
        buy.quantity -= quantity;
        sell.quantity -= quantity;
        remaining -= quantity;
        if (buy.quantity == 0) {
            bid_level->second.pop_front();
            if (bid_level->second.empty()) {
                bids_.erase(bid_level);
            }
        }
        if (sell.quantity == 0) {
            ask_level->second.pop_front();
            if (ask_level->second.empty()) {
                asks_.erase(ask_level);
            }
        }
    }
    return result;
}

std::vector<Order> OrderBook::remove_if(const std::function<bool(const Order&)>& which) {
    std::unique_lock lock(mutex_);
    std::vector<Order> removed;
    auto sweep = [&](auto& side) {
        for (auto level = side.begin(); level != side.end();) {
            auto& queue = level->second;
            for (auto it = queue.begin(); it != queue.end();) {
                if (which(*it)) {
                    removed.push_back(std::move(*it));
                    it = queue.erase(it);
                } 
                else {
                    ++it;
                }
            }
            level = queue.empty() ? side.erase(level) : std::next(level);
        }
    };
    sweep(bids_);
    sweep(asks_);
    return removed;
}

} // namespace sim
