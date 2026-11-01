//=======================================================================
// Stores orders that passed risk checks but are not active in the order book yet.
// This includes STOP/LIMIT_STOP orders waiting for their release price and orders with a future `not_before` start time.
// The registry has its own mutex so waiting-order scans do not block order matching or settlement.
// Orders are grouped by symbol: after a trade, the MatchingEngine only scans the waiting orders for that symbol instead of scanning the entire registry. 
// A full scan is also available for periodic checks, mainly to release or expire orders that depend only on time (for example, `not_before` or `expires_at`)
//=======================================================================
#ifndef ORDER_REGISTRY_HPP
#define ORDER_REGISTRY_HPP

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "types.hpp"

namespace sim {

class OrderRegistry {
public:
    // Adds an order to the waiting pool, bucketed by order.symbol
    void add(Order order);

    // Removes a specific waiting order (administrative cancel) by id alone
    std::optional<Order> cancel(OrderId order_id, std::optional<ClientId> owner = std::nullopt);

    struct ScanResult {
        std::vector<Order> released;
        std::vector<Order> expired;
    };

    // Scans every waiting order across every symbol, for periodic sweeps
    // must catch orders waiting purely on a start date (no price-driven trigger) and as a correctness backstop
    template <typename ReferencePriceFn>
    ScanResult scan_and_release(TimePoint now, ReferencePriceFn get_reference_price) {
        std::lock_guard<std::mutex> lock(mutex_);
        ScanResult result;
        for (auto& [symbol, bucket] : waiting_by_symbol_) {
            scan_bucket_locked(bucket, now, get_reference_price(symbol), result);
        }
        return result;
    }

    // Scans only `symbol`'s waiting orders: MatchingEngine calls it right after a trade updates that symbol's reference price, 
    //since that's exactly the moment a STOP/LIMIT_STOP order's release band is most likely to have just become satisfied
    template <typename ReferencePriceFn>
    ScanResult scan_and_release_symbol(const Symbol& symbol, TimePoint now, ReferencePriceFn get_reference_price) {
        std::lock_guard<std::mutex> lock(mutex_);
        ScanResult result;
        auto it = waiting_by_symbol_.find(symbol);
        if (it != waiting_by_symbol_.end()) {
            scan_bucket_locked(it->second, now, get_reference_price(symbol), result);
        }
        return result;
    }

    std::size_t size() const;
    std::vector<Symbol> symbols_with_waiting_orders() const;

    // Copies of every order currently waiting (all symbols)
    std::vector<Order> all_waiting() const;

private:
    using Bucket = std::unordered_map<OrderId, Order>;

    // shared scan body for both public scan methods: caller already holds mutex_
    template <typename ScanResultT>
    void scan_bucket_locked(Bucket& bucket, TimePoint now, Price reference_price, ScanResultT& result) {
        for (auto it = bucket.begin(); it != bucket.end();) {
            Order& order = it->second;

            if (order.expires_at.has_value() && now >= *order.expires_at) {
                result.expired.push_back(std::move(order));
                order_symbol_index_.erase(it->first);
                it = bucket.erase(it);
                continue;
            }

            bool started = !order.not_before.has_value() || now >= *order.not_before;
            if (started && order.release_band_contains(reference_price)) {
                result.released.push_back(std::move(order));
                order_symbol_index_.erase(it->first);
                it = bucket.erase(it);
                continue;
            }
            ++it;
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<Symbol, Bucket> waiting_by_symbol_;
    std::unordered_map<OrderId, Symbol> order_symbol_index_; // O(1) cancel-by-id
};

} // namespace sim

#endif // ORDER_REGISTRY_HPP