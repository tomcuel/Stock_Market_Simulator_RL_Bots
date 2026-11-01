#include "order_registry.hpp"

namespace sim {

void OrderRegistry::add(Order order) {
    std::lock_guard<std::mutex> lock(mutex_);
    OrderId id = order.id;
    Symbol symbol = order.symbol;
    waiting_by_symbol_[symbol].emplace(id, std::move(order));
    order_symbol_index_.emplace(id, std::move(symbol));
}

std::optional<Order> OrderRegistry::cancel(OrderId order_id, std::optional<ClientId> owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto index_it = order_symbol_index_.find(order_id);
    if (index_it == order_symbol_index_.end()) {
        return std::nullopt;
    }

    std::optional<Order> removed;
    auto bucket_it = waiting_by_symbol_.find(index_it->second);
    if (bucket_it != waiting_by_symbol_.end()){
        auto order_it = bucket_it->second.find(order_id);
        if (order_it != bucket_it->second.end() && owner && order_it->second.client != *owner){
            return std::nullopt; // someone else's order: left untouched
        }
        if (order_it != bucket_it->second.end()){
            removed = std::move(order_it->second);
            bucket_it->second.erase(order_it);
        }
    }

    order_symbol_index_.erase(index_it);
    return removed;
}

std::size_t OrderRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return order_symbol_index_.size();
}

std::vector<Symbol> OrderRegistry::symbols_with_waiting_orders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Symbol> symbols;
    for (const auto& [symbol, bucket] : waiting_by_symbol_) {
        if (!bucket.empty()) symbols.push_back(symbol);
    }
    return symbols;
}

std::vector<Order> OrderRegistry::all_waiting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Order> result;
    result.reserve(order_symbol_index_.size());
    for (const auto& [symbol, bucket] : waiting_by_symbol_) {
        for (const auto& [id, order] : bucket) {
            result.push_back(order);
        }
    }
    return result;
}

} // namespace sim
