#include "market_data.hpp"

#include <algorithm>

namespace sim {

MarketData::MarketData(NotificationBus& bus, std::chrono::milliseconds bucket_size)
    : bus_(bus), bucket_size_(bucket_size) {
    subscription_id_ = bus_.subscribe([this](const Event& event) { on_event(event); });
}

MarketData::~MarketData() {
    // Unsubscribes from `bus_` before any of this object's own state is torn down
    bus_.unsubscribe(subscription_id_);
}

void MarketData::on_event(const Event& event) {
    if (auto* trade_event = std::get_if<TradeEvent>(&event)) {
        on_trade(trade_event->trade);
    }
}

void MarketData::on_trade(const Trade& trade) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = state_[trade.symbol];
    state.last_price = trade.price;
    state.total_volume += trade.quantity;
    state.cumulative_notional += static_cast<double>(trade.quantity) * trade.price;

    // bucket the trade's timestamp into a fixed-size candle window
    auto since_epoch = trade.timestamp.time_since_epoch();
    auto bucket_index = since_epoch / bucket_size_;
    TimePoint bucket_start = TimePoint(bucket_index * bucket_size_);

    if (state.candles.empty() || state.candles.back().bucket_start != bucket_start) {
        Candle candle;
        candle.bucket_start = bucket_start;
        candle.open = candle.high = candle.low = candle.close = trade.price;
        candle.volume = trade.quantity;
        state.candles.push_back(candle);
    } 
    else {
        Candle& candle = state.candles.back();
        candle.high = std::max(candle.high, trade.price);
        candle.low = std::min(candle.low, trade.price);
        candle.close = trade.price;
        candle.volume += trade.quantity;
    }
}

Price MarketData::last_price(const Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(symbol);
    return it == state_.end() ? 0.0 : it->second.last_price;
}

Quantity MarketData::total_volume(const Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(symbol);
    return it == state_.end() ? 0 : it->second.total_volume;
}

double MarketData::vwap(const Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(symbol);
    if (it == state_.end() || it->second.total_volume == 0) {
        return 0.0;
    }
    return it->second.cumulative_notional / static_cast<double>(it->second.total_volume);
}

std::vector<Candle> MarketData::recent_candles(const Symbol& symbol, std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(symbol);
    if (it == state_.end()) {
        return {};
    }
    const auto& candles = it->second.candles;
    std::size_t n = std::min(count, candles.size());
    return std::vector<Candle>(candles.end() - static_cast<long>(n), candles.end());
}

} // namespace sim
