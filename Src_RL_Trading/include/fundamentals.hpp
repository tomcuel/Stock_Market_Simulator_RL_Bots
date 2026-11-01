//=======================================================================
// Fundamental values: what each company is worth, the anchor of a real stock market (simulation.x --value N)
//
// Without an anchor the simulated price is whatever the bots' order flow makes it: it drifted up by about 1% a day and swung 4 to 8% a day, mean-reverting from one day to the next, 
// while the real tickers it starts from drift about +0.05% a day, move 1 to 2.3% and show no day-to-day autocorrelation
//
// Each symbol's value follows a random walk on log prices, calibrated on the ticker's real volatility (the seed's 20-day annualized volatility / sqrt(252) per day):
//   overnight   one shock at each open, overnight_share of the daily variance: the news that arrives while the market is closed, it also moves the reference price, so the day opens with a gap (a real day: 27 to 50% of the variance)
//   intraday    the rest, spread over the trading hours in small steps (diffuse)
//   drift       drift_per_day (default 0.0005: +5 bp a day, the real tickers' mean)
// The value investors (ValueInvestorBot) each see the value with their own persistent estimation error, buy below it and sell above it: they pull the price toward the value without ever matching it exactly
//=======================================================================
#ifndef FUNDAMENTALS_HPP
#define FUNDAMENTALS_HPP

#include <cmath>
#include <mutex>
#include <random>
#include <unordered_map>

#include "types.hpp"

namespace sim {

class FundamentalValues {
public:
    FundamentalValues(unsigned seed, double overnight_share = 0.35, double drift_per_day = 0.0005) : rng_(seed), overnight_share_(std::clamp(overnight_share, 0.0, 1.0)), drift_(drift_per_day) {}

    // daily_vol: standard deviation of the daily log return (0.0126 for a 20% annualized volatility)
    void add(const Symbol& symbol, Price value, double daily_vol) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_[symbol] = State{std::log(std::max(0.01, value)), std::max(0.0, daily_vol)};
    }

    Price value(const Symbol& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = state_.find(symbol);
        return it == state_.end() ? 0.0 : std::exp(it->second.log_value);
    }

    double daily_vol(const Symbol& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = state_.find(symbol);
        return it == state_.end() ? 0.0 : it->second.daily_vol;
    }

    // `fraction` of a trading day went by: the intraday part of the variance and of the drift
    void diffuse(double fraction) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::normal_distribution<double> z(0.0, 1.0);
        for (auto& [symbol, s] : state_) {
            double sigma = s.daily_vol * std::sqrt((1.0 - overnight_share_) * fraction);
            s.log_value += drift_ * fraction - 0.5 * sigma * sigma + sigma * z(rng_);
        }
    }

    // the night's news, at the open: returns each symbol's overnight log shock (also applied to its value)
    std::unordered_map<Symbol, double> overnight() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::normal_distribution<double> z(0.0, 1.0);
        std::unordered_map<Symbol, double> shocks;
        for (auto& [symbol, s] : state_) {
            double sigma = s.daily_vol * std::sqrt(overnight_share_);
            double shock = sigma * z(rng_);
            s.log_value += shock;
            shocks[symbol] = shock;
        }
        return shocks;
    }

private:
    struct State {
        double log_value{0.0};
        double daily_vol{0.0};
    };
    mutable std::mutex mutex_;
    std::mt19937 rng_;
    double overnight_share_;
    double drift_;
    std::unordered_map<Symbol, State> state_;
};

} // namespace sim

#endif // FUNDAMENTALS_HPP
