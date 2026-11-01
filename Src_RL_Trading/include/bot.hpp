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
//   value          value investor (simulation.x --value): buys below the symbol's fundamental value and sells above it, each with its own persistent estimation error, passively when the gap is small, at market when it is large (fundamentals.hpp)
//   twap           execution algorithm: works a large parent order in time slices at the touch, crosses the spread only when behind schedule, logs its slippage vs arrival price
// The RL bots (RLBot, rl_bot.hpp) are TradingStrategies too, kept apart so the socket bots (sim_client.x) never link them
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

#include "fundamentals.hpp"
#include "gateway.hpp"
#include "market_session.hpp"
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
    // With share counts, "10 shares" meant 8700 on ^AEX and 425000 on ^DJI, far above a bot's cash, so most buys of expensive symbols were refused while every sell passed, and the order flow was structurally sell-heavy
    static constexpr double kLotFraction = 0.002;
    Quantity lots(Price price, double units) const;
    double lot_value() const {return lot_value_;}

    // records one last price per symbol (keeps the `keep` most recent), used by strategies that need a history: momentum, meanreversion, trend
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

private:
    // the side leans toward the account's starting mix: a noise trader that sold most of a symbol's shares (or spent most of its cash) buys (sells) it more often
    // With a fair coin, sells of a symbol whose shares ran out were refused while buys kept drawing on the shared cash: a structural buying pressure that pushed every price up by about 1% a day
    double buy_probability(const Symbol& symbol, Price price);
    double start_cash_{-1.0};
    std::unordered_map<Symbol, double> start_value_; // the symbol's starting holding, valued at the price of the first look
};


class ValueInvestorBot : public TradingStrategy {
public:
    ValueInvestorBot(MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities, const FundamentalValues* values)
        : TradingStrategy(gateway, std::move(symbols), seed, std::move(volatilities)), values_(values) {}
    const char* name() const override {return "value";}

protected:
    void on_tick() override;

private:
    const FundamentalValues* values_;
    std::unordered_map<Symbol, double> error_; // this investor's persistent log error on each symbol's value
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
    // how far from the mid it quotes, as a fraction of the mid (default 0.5%: a 1% quoted spread). simulation.x --maker-half-spread
    void set_half_spread(double half_spread) {half_spread_ = std::clamp(half_spread, 0.0001, 0.05);}
    static constexpr double kHalfSpread = 0.005;

protected:
    void on_tick() override;

private:
    double half_spread_{kHalfSpread};
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


// the value investors see each company's hidden fundamental value, which no other bot sees: a
// n informed benchmark, reported apart from the strategies that only see prices (Simulation's report, scripts/plot_market_report.py, plot_rl_report.py)
inline bool is_informed_benchmark(const std::string& strategy) {return strategy == "value";}

// every strategy name accepted by make_strategy(), in a stable order
const std::vector<std::string>& strategy_names();

// builds a rule-based strategy by name (see strategy_names()), nullptr for an unknown name
// The RL bots need a policy server connection and are built directly (RLBot, rl_bot.hpp), see Simulation::spawn_rl_bots
std::unique_ptr<TradingStrategy> make_strategy(const std::string& name, MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities = {});

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
    // trading days: the bot only steps while the exchange accepts orders (it sleeps through the night), set before start()
    void set_session(const MarketSession* session) {session_ = session;}

private:
    std::unique_ptr<TradingStrategy> strategy_;
    std::chrono::milliseconds min_interval_;
    std::chrono::milliseconds max_interval_;
    std::mt19937 rng_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    const MarketSession* session_{nullptr};
};

} // namespace sim

#endif // BOT_HPP
