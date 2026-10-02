//=======================================================================
// Client "bots" that act on the market to generate realistic, concurrent trading activity for the simulation, 
// exactly the way a real exchange gets exercised by many independent participants rather than a couple of manually-typed test orders
// A bot is not pinned to a single symbol: it holds the full list of tradable symbols and, on each tick, picks one to apply its strategy
//=======================================================================
#pragma once

#include <atomic>
#include <deque>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "market_data.hpp"
#include "matching_engine.hpp"
#include "types.hpp"

namespace sim {

class TradingBot {
public:
    TradingBot(MatchingEngine& engine, const MarketData& market_data, ClientId client, std::vector<Symbol> symbols,
               std::chrono::milliseconds min_interval, std::chrono::milliseconds max_interval, unsigned seed,
               std::unordered_map<Symbol, double> volatilities = {})
        : engine_(engine), market_data_(market_data), client_(client), symbols_(std::move(symbols)),
        min_interval_(min_interval), max_interval_(max_interval), rng_(seed), volatilities_(std::move(volatilities)) {}

    virtual ~TradingBot() = default;

    void start() {
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ClientId client() const { return client_; }

    // subclasses implement their strategy: decide on and submit (at most a couple of) orders for whichever symbol pick_symbol() gives them this tick
    // Public (not just callable via the internal run() loop) so NRT tests can call it directly, deterministically, without needing a real background thread and real sleeps
    virtual void step() = 0;

protected:
    // picks one symbol at random from the full list this bot can trade: called once per step() so each tick's strategy decision is about a (possibly different) symbol than the last
    Symbol pick_symbol() {
        std::uniform_int_distribution<std::size_t> dist(0, symbols_.size() - 1);
        return symbols_[dist(rng_)];
    }

    // Annualized volatility (e.g. 0.20 == 20%/year by default) for a specific symbol, ideally from real data
    // Scaled down heuristically by each strategy for its own per-tick price-offset range: this isn't meant to be a rigorous volatility model, 
    // just enough that a historically volatile ticker visibly trades more volatile here too
    double volatility_for(const Symbol& symbol) const {
        auto it = volatilities_.find(symbol);
        return it == volatilities_.end() ? 0.20 : it->second;
    }

    MatchingEngine& engine_;
    const MarketData& market_data_;
    ClientId client_;
    std::vector<Symbol> symbols_;
    std::chrono::milliseconds min_interval_;
    std::chrono::milliseconds max_interval_;
    std::mt19937 rng_;

private:
    void run() {
        std::uniform_int_distribution<int> jitter(static_cast<int>(min_interval_.count()), static_cast<int>(max_interval_.count()));
        while (running_) {
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds(jitter(rng_)));
        }
    }

    std::unordered_map<Symbol, double> volatilities_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};


// Trades randomly around the last price with small size: provides baseline liquidity and noise, the way most retail order flow behaves. 
class NoiseTraderBot : public TradingBot {
public:
    using TradingBot::TradingBot;

    void step() override;
};


// Follows the recent short-term trend: buys after an up-move, sells after a down-move
// Keeps its own small rolling window of observed last-prices per symbol (sampled once per tick), rather than reading MarketData's time-bucketed candles: 
// candles only form once enough wall-clock time has passed with trades in each bucket (1 second by default), which is far slower than an bot's own tick interval (as low as 200ms)
// in a short simulation a momentum bot reading candles could easily never see 2 of them form at all and would look "broken" (never trading) even with plenty of real price movement happening
class MomentumBot : public TradingBot {
public:
    using TradingBot::TradingBot;

    void step() override;

private:
    static constexpr std::size_t kWindowSize = 5;
    std::unordered_map<Symbol, std::deque<double>> price_history_;
};


// Quotes a bid and an ask around the current mid price with a small spread, providing continuous two-sided liquidity like a real market maker (simplified: no active inventory hedging)
// Rotates across symbols like every other bot (pick_symbol()): cancels its previous quotes before posting a fresh pair on the newly picked symbol
class MarketMakerBot : public TradingBot {
public:
    MarketMakerBot(MatchingEngine& engine, const MarketData& market_data, ClientId client, std::vector<Symbol> symbols,
                   std::chrono::milliseconds min_interval, std::chrono::milliseconds max_interval, unsigned seed,
                   std::unordered_map<Symbol, double> volatilities = {}, double spread_fraction = 0.01)
        : TradingBot(engine, market_data, client, std::move(symbols), min_interval, max_interval, seed, std::move(volatilities)),
          spread_fraction_(spread_fraction) {}

    void step() override;

private:
    double spread_fraction_;
    Symbol last_quote_symbol_;
    OrderId last_buy_order_{0};
    OrderId last_sell_order_{0};
};

} // namespace sim
