//=======================================================================
// Subscribes to trade events and builds fixed-interval OHLCV candles per symbol, plus basic running stats (last price, cumulative volume, VWAP)
// Bot / dashboard / future charting layer would read those instead of hammering the order book directly
//=======================================================================
#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include "notification.hpp"
#include "types.hpp"

namespace sim {

struct Candle {
    TimePoint bucket_start;
    Price open{0.0};
    Price high{0.0};
    Price low{0.0};
    Price close{0.0};
    Quantity volume{0};
};

class MarketData {
public:
    explicit MarketData(NotificationBus& bus, std::chrono::milliseconds bucket_size = std::chrono::seconds(1));
    // Unsubscribes from `bus_` before any of this object's own state is torn down
    ~MarketData();

    MarketData(const MarketData&) = delete;
    MarketData& operator=(const MarketData&) = delete;

    Price last_price(const Symbol& symbol) const;
    Quantity total_volume(const Symbol& symbol) const;
    double vwap(const Symbol& symbol) const;

    // most recent candles first (returns up to `count`)
    std::vector<Candle> recent_candles(const Symbol& symbol, std::size_t count = 10) const;

private:
    void on_event(const Event& event);
    void on_trade(const Trade& trade);

    NotificationBus& bus_;
    SubscriptionId subscription_id_;

    std::chrono::milliseconds bucket_size_;

    mutable std::mutex mutex_;
    struct SymbolState {
        Price last_price{0.0};
        Quantity total_volume{0};
        double cumulative_notional{0.0}; // for VWAP = cumulative_notional / total_volume
        std::vector<Candle> candles;
    };
    std::unordered_map<Symbol, SymbolState> state_;
};

} // namespace sim
