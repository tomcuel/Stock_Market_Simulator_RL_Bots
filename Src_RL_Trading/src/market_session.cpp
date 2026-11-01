#include "market_session.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace sim {

DailyBarBuilder::DailyBarBuilder(NotificationBus& bus) : bus_(bus) {
    subscription_ = bus_.subscribe([this](const Event& event) {
        const auto* trade = std::get_if<TradeEvent>(&event);
        if (!trade) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_day_) {
            return;
        }
        auto it = current_.find(trade->trade.symbol);
        if (it == current_.end()) {
            return;
        }
        DailyBar& bar = it->second;
        Price price = trade->trade.price;
        if (bar.trades == 0) {
            bar.open = bar.high = bar.low = price;
        }
        bar.high = std::max(bar.high, price);
        bar.low = std::min(bar.low, price);
        bar.close = price;
        bar.volume += trade->trade.quantity;
        notional_[bar.symbol] += price * static_cast<double>(trade->trade.quantity);
        ++bar.trades;
    });
}

DailyBarBuilder::~DailyBarBuilder() {
    bus_.unsubscribe(subscription_);
}

void DailyBarBuilder::start_day(int day, double now_ms, const std::unordered_map<Symbol, Price>& reference, const std::unordered_map<Symbol, double>& gaps) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.clear();
    notional_.clear();
    for (const auto& [symbol, price] : reference) {
        DailyBar bar;
        bar.day = day;
        bar.symbol = symbol;
        bar.open_ms = now_ms;
        bar.open = bar.high = bar.low = bar.close = price; // until the first trade
        auto gap = gaps.find(symbol);
        bar.gap = gap == gaps.end() ? 0.0 : gap->second;
        current_[symbol] = bar;
    }
    in_day_ = true;
}

void DailyBarBuilder::set_book(const Symbol& symbol, Price bid, Price ask, double bid_depth, double ask_depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = current_.find(symbol);
    if (it == current_.end()) {
        return;
    }
    it->second.bid = bid;
    it->second.ask = ask;
    it->second.bid_depth = bid_depth;
    it->second.ask_depth = ask_depth;
}

std::vector<DailyBar> DailyBarBuilder::finish_day(double now_ms, const std::unordered_map<Symbol, Price>& closes, const std::unordered_map<Symbol, Quantity>& auction_volumes) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DailyBar> bars;
    for (auto& [symbol, bar] : current_) {
        bar.close_ms = now_ms;
        auto close = closes.find(symbol);
        if (close != closes.end() && close->second > 0.0) {
            bar.close = close->second; // the official close: the auction's price (its trades are already in the bar)
            bar.high = std::max(bar.high, bar.close);
            bar.low = std::min(bar.low, bar.close);
        }
        auto auction = auction_volumes.find(symbol);
        bar.auction_volume = auction == auction_volumes.end() ? 0 : auction->second;
        bar.vwap = bar.volume > 0 ? notional_[symbol] / static_cast<double>(bar.volume) : bar.close;
        bars.push_back(bar);
    }
    std::sort(bars.begin(), bars.end(), [](const DailyBar& a, const DailyBar& b) {return a.symbol < b.symbol;});
    done_.insert(done_.end(), bars.begin(), bars.end());
    in_day_ = false;
    return bars;
}

std::vector<DailyBar> DailyBarBuilder::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
}

bool DailyBarBuilder::write_csv(const std::string& dir, const std::string& suffix) const {
    std::ofstream out(std::filesystem::path(dir) / ("daily_bars" + suffix + ".csv"));
    if (!out.is_open()) {
        return false;
    }
    out << "day,symbol,open_ms,close_ms,open,high,low,close,volume,vwap,trades,bid,ask,bid_depth,ask_depth,auction_volume,gap\n";
    out << std::setprecision(12);
    for (const auto& b : all()) {
        out << b.day << ',' << b.symbol << ',' << b.open_ms << ',' << b.close_ms << ',' << b.open << ',' << b.high << ',' << b.low << ',' << b.close << ','
            << b.volume << ',' << b.vwap << ',' << b.trades << ',' << b.bid << ',' << b.ask << ',' << b.bid_depth << ',' << b.ask_depth << ','
            << b.auction_volume << ',' << b.gap << '\n';
    }
    return true;
}

} // namespace sim
