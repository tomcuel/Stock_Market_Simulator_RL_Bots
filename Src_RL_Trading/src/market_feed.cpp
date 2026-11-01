#include "market_feed.hpp"

namespace sim {

bool MarketFeed::connect(const std::string& host, int port) {
    return client_.connect(host, port);
}

void MarketFeed::push_bars(const std::vector<DailyBar>& bars) {
    for (const auto& bar : bars) {
        ObservedBar observed;
        observed.close = bar.close;
        observed.bid = bar.bid;
        observed.ask = bar.ask;
        observed.bid_depth = bar.bid_depth;
        observed.ask_depth = bar.ask_depth;
        observed.volume = static_cast<double>(bar.volume);
        observed.open = bar.open;
        observed.high = bar.high;
        observed.low = bar.low;
        observed.trades = static_cast<double>(bar.trades);
        if (observed.close > 0.0 && client_.observe(bar.symbol, bar.day, observed)) sent_.fetch_add(1);
    }
    days_.fetch_add(1);
}

} // namespace sim
