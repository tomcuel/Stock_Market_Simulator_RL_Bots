//=======================================================================
// The RL bots: each one trades what a walk-forward-trained PPO agent of RL_Agents/ (served by the Python policy server, policy_client.hpp) chooses once per trading day: 
// an entry at the open, a GTC stop-loss and a GTC take-profit (a bracket) simulation.x only: the socket bots of sim_client.x are rule-based (bot.hpp)
//=======================================================================
#ifndef RL_BOT_HPP
#define RL_BOT_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bot.hpp"
#include "market_session.hpp"
#include "policy_client.hpp"

namespace sim {

// One trade of an RL bot, as it really went in the market
struct RLTrade {
    Symbol symbol;
    int direction{0};
    int entry_day{0};
    int exit_day{0};
    Quantity quantity{0};
    Price entry{0.0};   // the entry's average fill (the opening auction)
    Price exit{0.0};    // the exit's average fill (the stop's, the target's, or the closing order's)
    Price stop{0.0};
    Price target{0.0};
    double sl_atr{0.0};
    double tp_r{0.0};
    std::string reason; // TP, SL, close (the agent went flat), flip (it reversed), both (stop and target filled: corrected at market)
    double r() const {double risk = std::abs(entry - stop); return risk > 0.0 ? (exit - entry) * direction / risk : 0.0;}
};

// In each trading day's pre-open, for every symbol, it sends the agent its open trade (ACT <model> <symbol> dir= entry= sl= tp= days= tpr=) 
// and follows the answer with market-on-open orders, which fill in the opening auction at the open, 
// the price the agent was trained to trade at (a market order sent just after the auction paid a median 25 bp and a mean 141 bp over the open, in a book the auction had just emptied):
//   same direction        hold: the stop and the target keep resting (GTC, they carry overnight)
//   flat or the other way out at the open (both legs cancelled first), then maybe in the other way
//   a new direction       an entry sized so the stop risks risk_fraction (0.2%) of the equity, capped at max_notional (0.5%) of it (a short sells the starting inventory). At the first tick after the auction: a STOP at the stop-loss (a stop-limit 1% through it, exited at market past that) and a LIMIT at the take-profit, both GTC
// Every tick it watches its holdings: once the position is gone, one leg has filled, the other is cancelled (one-cancels-other);
// every price it records is a real fill (its gateway tracks its fills from the exchange's trade events)
class RLBot : public TradingStrategy {
public:
    static constexpr double kRiskFraction = 0.002;
    static constexpr double kMaxNotional = 0.005; // 50k for a 10-million account: about one typical trade of the simulated book (at 3%, a stop walked it 1 to 2% down)
    // the stop's limit, 1% through the stop (a stop-limit; watch() exits at market when a gap goes past it)
    // Measured live: a stop-market (its limit at the 10% collar) filled a median 327 bp past the stop, in the cascade of every stop the overnight gap released at the open; this one a median 97 bp, -1.31 R per stop-loss instead of -1.86
    static constexpr double kStopLimitThrough = 0.01;

    RLBot(MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities, std::shared_ptr<PolicyClient> client, PolicyInfo model, double risk_fraction = kRiskFraction, double max_notional = kMaxNotional) : TradingStrategy(gateway, std::move(symbols), seed, std::move(volatilities)), client_(std::move(client)), model_(std::move(model)), risk_fraction_(risk_fraction), max_notional_(max_notional) {}
    const char* name() const override {return model_.name.c_str();}
    void set_session(const MarketSession* session) {session_ = session;}
    const std::vector<RLTrade>& trades() const {return trades_;}
    std::uint64_t days_seen() const {return days_;}
    std::uint64_t decisions() const {return decisions_;}
    std::uint64_t entries() const {return entries_;}
    std::uint64_t waited() const {return waited_;}
    std::uint64_t failures() const {return failures_;}
    std::uint64_t legs_rejected() const {return legs_rejected_;}
    std::uint64_t missed_opens() const {return missed_;}
    int open_direction(const Symbol& symbol) const {auto it = open_.find(symbol); return it == open_.end() ? 0 : it->second.direction;}

protected:
    void on_tick() override;

private:
    struct Open {
        int direction{0};
        Quantity quantity{0};
        Quantity base{0};     // the holding before the entry: the position is gone when the holding is back to it
        Price entry{0.0}, stop{0.0}, target{0.0};
        double sl_atr{0.0}, tp_r{0.0};
        int days{0}, entry_day{0};
        OrderId stop_id{0}, target_id{0};
    };
    void watch(const Symbol& symbol, Open& open, const Portfolio& portfolio, int day);
    void decide(const Symbol& symbol);
    void settle_auction(int day);
    void place_legs(const Symbol& symbol, Open& open, int day);
    void close_at_market(const Symbol& symbol, Open& open, const std::string& reason, int day);
    void record(const Symbol& symbol, const Open& open, Price exit, const std::string& reason, int day);

    std::shared_ptr<PolicyClient> client_;
    PolicyInfo model_;
    double risk_fraction_;
    double max_notional_;
    const MarketSession* session_{nullptr};
    int last_day_{-1};
    double equity_{0.0};
    std::unordered_map<Symbol, Open> open_;
    // the day's orders sent in the pre-open, waiting for the opening auction
    struct Pending {
        int direction{0};               // a new trade's direction (0: none)
        double sl_dist{0.0}, tp_r{0.0}, sl_atr{0.0};
        Quantity base{0};               // the holding the new trade (or the close) is measured from
        Quantity holding_before{0};     // the holding when the orders were sent
        std::string close_reason;       // "close" or "flip" when an open trade goes at the open
        OrderId entry_id{0}, close_id{0};
    };
    std::unordered_map<Symbol, Pending> pending_;
    int settled_day_{-1};
    std::uint64_t missed_{0};
    std::vector<RLTrade> trades_;
    std::uint64_t days_{0}, decisions_{0}, entries_{0}, waited_{0}, failures_{0}, legs_rejected_{0};
};

} // namespace sim

#endif // RL_BOT_HPP
