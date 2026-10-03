//=======================================================================
// Trading bots: strategies that act on the market through a MarketGateway (see gateway.hpp), so the same code runs in-process (simulation.x) and over TCP (sim_client.x --bot)
//
// Every strategy inherits TradingStrategy, which gives it:
//   - the whole market: a bot is never pinned to one symbol, pick_symbol() draws one per tick
//   - price memory: observe_all() records every symbol's last price, per symbol, once per tick
//   - order management: place() remembers every order that may still be open, and every tick the base class retracts the ones older than their maximum age (retract_stale)$
//                       Stale orders would otherwise pile up in the book and, since open orders reserve cash and shares (see portfolio.hpp), slowly lock the whole account
// Strategies:
//   noise          random side and order kind around the last price: baseline liquidity
//   momentum       follows a significant recent move with a MARKET order, protected by an opposite STOP, with a per-symbol cooldown
//   marketmaker    two-sided quotes around the mid, leaning with inventory (Avellaneda-Stoikov idea: long means lower quotes to sell, short means higher quotes to buy back), requoted every tick, never crossing the mid
//   meanreversion  z-score of the price against its rolling mean: buys well below, sells well above, with passive LIMIT orders that expire quickly
//   trend          fast/slow exponential moving average crossover, enters with MARKET orders and keeps a trailing STOP that it retracts and re-places as the price moves its way
//   twap           execution algorithm: works a large parent order in time slices at the touch, crosses the spread only when behind schedule, logs its slippage vs arrival price
//   ml             logistic regression signal trained in Python (scripts/train_signal_model.py), inferred in C++ (signal_model.hpp)
//=======================================================================
#ifndef BOT_HPP
#define BOT_HPP

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gateway.hpp"
#include "signal_model.hpp"
#include "types.hpp"

namespace sim {

class TradingStrategy {
public:
    TradingStrategy(MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities = {});
    virtual ~TradingStrategy() = default;

    // one decision round: retracts stale orders, then lets the strategy act (on_tick): alled on a timer by BotRunner, by sim_client.x's bot loop, or directly by the NRT tests
    void step();

    virtual const char* name() const = 0;
    ClientId client() const {return gateway_.client();}
    std::size_t open_order_count() const {return open_orders_.size();}
    std::uint64_t orders_placed() const {return placed_;}
    std::uint64_t orders_retracted() const {return retracted_;}

protected:
    virtual void on_tick() = 0;

    Symbol pick_symbol();
    double volatility_for(const Symbol& symbol) const;
    // per-order price offset scale from annualized volatility: vol/20, bounded to [0.2%, 10%]
    double offset_scale(const Symbol& symbol) const;

    // Orders are sized by value, not by share count: `units` lots at `price`, one lot being kLotFraction of the bot's equity when it started (so 20000 with the default 10000000), converted to shares and never less than 1
    //With share counts, "10 shares" meant 8700 on ^AEX and 425000 on ^DJI, far above a bot's cash, so most buys of expensive symbols were refused while every sell passed, and the order flow was structurally sell-heavy
    static constexpr double kLotFraction = 0.002;
    Quantity lots(Price price, double units) const;
    double lot_value() const {return lot_value_;}

    // records one last price per symbol (keeps the `keep` most recent), used by strategies that need a history: momentum, meanreversion, trend, ml
    void observe_all(std::size_t keep);
    const std::deque<double>& history(const Symbol& symbol) {return history_[symbol];}

    // submits through the gateway and remembers the order if it may still be open (resting or waiting): max_age_ticks = how many ticks it may live before retract_stale() cancels it
    SubmitResult place(OrderRequest request, int max_age_ticks);
    bool retract(OrderId order_id);
    void retract_all(const Symbol& symbol);

    MarketGateway& gateway_;
    std::vector<Symbol> symbols_;
    std::mt19937 rng_;
    std::uint64_t tick_{0};

private:
    struct OpenOrder
    {
        Symbol symbol;
        OrderId id;
        std::uint64_t expires_at_tick;
    };
    void retract_stale();

    std::unordered_map<Symbol, double> volatilities_;
    std::unordered_map<Symbol, std::deque<double>> history_;
    std::vector<OpenOrder> open_orders_;
    std::uint64_t placed_{0};
    std::uint64_t retracted_{0};
    double lot_value_{0.0}; // measured on the first tick, see lots()
};


class NoiseTraderBot : public TradingStrategy {
public:
    using TradingStrategy::TradingStrategy;
    const char* name() const override {return "noise";}

protected:
    void on_tick() override;
};


class MomentumBot : public TradingStrategy {
public:
    using TradingStrategy::TradingStrategy;
    const char* name() const override {return "momentum";}

protected:
    void on_tick() override;

private:
    std::unordered_map<Symbol, std::uint64_t> last_trade_tick_; // per-symbol cooldown
};


class MarketMakerBot : public TradingStrategy {
public:
    using TradingStrategy::TradingStrategy;
    const char* name() const override {return "marketmaker";}

protected:
    void on_tick() override;

private:
    std::unordered_map<Symbol, Quantity> target_position_; // first holding seen: the inventory it quotes around
    std::vector<OrderId> live_quotes_;
};


class MeanReversionBot : public TradingStrategy {
public:
    using TradingStrategy::TradingStrategy;
    const char* name() const override {return "meanreversion";}

protected:
    void on_tick() override;
};


class TrendFollowerBot : public TradingStrategy {
public:
    using TradingStrategy::TradingStrategy;
    const char* name() const override {return "trend";}

protected:
    void on_tick() override;

private:
    struct TrendState {
        double fast{0.0};
        double slow{0.0};
        int samples{0};
        int direction{0};       // +1 long, -1 short, 0 flat
        OrderId stop_id{0};     // current trailing stop
        Price stop_price{0.0};
        Quantity quantity{0};   // size of the current position the stop protects
    };
    std::unordered_map<Symbol, TrendState> trends_;
};


class TwapExecutionBot : public TradingStrategy {
public:
    using TradingStrategy::TradingStrategy;
    const char* name() const override {return "twap";}
    std::uint64_t parents_completed() const {return parents_completed_;}

protected:
    void on_tick() override;

private:
    struct Parent {
        bool active{false};
        Symbol symbol;
        Side side{Side::BUY};
        Quantity total{0};
        int horizon_ticks{0};
        std::uint64_t started_tick{0};
        Price arrival_price{0.0};
        Quantity start_holding{0};
        double start_cash{0.0};
        OrderId child_id{0};
    };
    void start_parent();
    void finish_parent(Quantity filled, double cash_now);
    Parent parent_;
    std::uint64_t parents_completed_{0};
};


// The model learned from DAILY closes (Data/ pipeline), the simulation moves tick by tick: the bot reads the market in bars
// One bar of bar_interval simulated time plays one trading day: at each bar close it records one close per symbol and decides once per symbol, so "5 bars ahead" means what "5 days ahead" meant in training
// Between two bar closes it does nothing (bar_interval 0: every tick, the old behavior), scripts/train_signal_model.py --check-run tells which bar length makes the simulated moves look like real days
class MLSignalBot : public TradingStrategy {
public:
    MLSignalBot(MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities, std::shared_ptr<const SignalModel> model, std::chrono::milliseconds bar_interval = std::chrono::milliseconds(1000)) : TradingStrategy(gateway, std::move(symbols), seed, std::move(volatilities)), model_(std::move(model)), bar_interval_(bar_interval) {}
    const char* name() const override {return "ml";}
    std::uint64_t bars_seen() const {return bars_;}
    std::uint64_t positions_opened() const {return opened_;}
    std::uint64_t horizon_exits() const {return horizon_exits_;} // closed by the bot after horizon() bars
    std::uint64_t stop_exits() const {return stop_exits_;}       // entirely closed earlier by their protective stop
    std::size_t open_positions() const {return positions_.size();}

protected:
    void on_tick() override;

private:
    struct Position {
        Side side;
        Quantity quantity;
        OrderId stop_id;            // 0 if the stop couldn't be placed
        std::uint64_t opened_bar;
        Quantity holding_before;    // the holding just before the entry: what is still open is the difference
    };
    void on_bar(const std::vector<Symbol>& symbols);
    void close_if_due(const Symbol& symbol);
    void decide(const Symbol& symbol);

    std::shared_ptr<const SignalModel> model_; // loaded once, shared by every ML bot
    std::chrono::milliseconds bar_interval_;
    std::int64_t last_bar_index_{-1};
    std::uint64_t bars_{0};
    std::unordered_map<Symbol, Position> positions_;
    std::uint64_t opened_{0};
    std::uint64_t horizon_exits_{0};
    std::uint64_t stop_exits_{0};
    static constexpr int kHoldUntilHorizon = 1000000; // ticks: the stop is retracted by the horizon exit, not by age
};


// every strategy name accepted by make_strategy(), in a stable order
const std::vector<std::string>& strategy_names();

// builds a strategy by name (see strategy_names()), nullptr for an unknown name or for "ml" without a loaded model
// ml_bar_interval: the ML bot's bar, one simulated trading day (see MLSignalBot)
std::unique_ptr<TradingStrategy> make_strategy(const std::string& name, MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities = {}, std::shared_ptr<const SignalModel> model = nullptr, std::chrono::milliseconds ml_bar_interval = std::chrono::milliseconds(1000));

// runs a strategy on its own thread, calling step() every [min_interval, max_interval]
class BotRunner
{
public:
    BotRunner(std::unique_ptr<TradingStrategy> strategy, std::chrono::milliseconds min_interval, std::chrono::milliseconds max_interval, unsigned seed);
    ~BotRunner();

    BotRunner(const BotRunner&) = delete;
    BotRunner& operator=(const BotRunner&) = delete;

    void start();
    void stop();
    TradingStrategy& strategy() {return *strategy_;}

private:
    std::unique_ptr<TradingStrategy> strategy_;
    std::chrono::milliseconds min_interval_;
    std::chrono::milliseconds max_interval_;
    std::mt19937 rng_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace sim

#endif // BOT_HPP
