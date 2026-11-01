#ifndef SIMULATION_HPP
#define SIMULATION_HPP

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "bot.hpp"
#include "rl_bot.hpp"
#include "fundamentals.hpp"
#include "market_data.hpp"
#include "market_feed.hpp"
#include "market_recorder.hpp"
#include "market_session.hpp"
#include "matching_engine.hpp"
#include "notification.hpp"
#include "types.hpp"

namespace sim {

struct SymbolConfig {
    Symbol symbol;
    Price initial_price;
    double volatility{0.20};
};

struct SimulationConfig {
    std::vector<SymbolConfig> symbols;
    int noise_traders{15};
    int momentum_traders{5};
    int market_makers{4};
    int mean_reversion_traders{4};
    int trend_followers{4};
    int twap_executors{2};
    // value investors (fundamentals.hpp): with value_investors > 0 every symbol gets a fundamental value, a random walk on the ticker's real volatility, overnight_share of its variance arriving as overnight news (the gap at the open)
    int value_investors{0};
    double maker_half_spread{MarketMakerBot::kHalfSpread}; // the market makers' quotes: mid -/+ this fraction
    double fundamental_overnight_share{0.35};
    double fundamental_drift{0.0005};
    // the RL agents of RL_Agents/ (policy_client.hpp): a policy server on rl_port (0: none). Each agent trades as its own RLBots, deciding once per trading day, the server fed each day's bars by a MarketFeed
    std::string rl_host{"127.0.0.1"};
    int rl_port{0};
    int rl_bots_per_model{2};
    double rl_risk_fraction{RLBot::kRiskFraction}; // an RL bot's risk per trade: the share of its equity its stop-loss loses
    // every bot starts with the same equity, bot_cash_fraction of it in cash and the rest split equally by value across the symbols (MatchingEngine::fund_balanced_account)
    // 10 million keeps whole shares fine-grained even for index-priced symbols (^DJI around 42000: 23 shares instead of 2)
    double bot_equity{10000000.0};
    double bot_cash_fraction{0.5};
    std::chrono::milliseconds bot_min_interval{20};
    std::chrono::milliseconds bot_max_interval{200};
    // trading days (market_session.hpp), the only mode: the run is session.days days (pre-open, opening auction, continuous trading, pre-close, closing auction, night) and lasts session.total(); the RL agents decide once per day
    SessionConfig session;
    unsigned seed{42};
    // Run output : when output_dir is set (created if missing), the simulation writes into it:
    //   metrics<file_suffix>.csv  one row of throughput/latency counters every metrics_interval (plot with scripts/plot_metrics.py)
    //   *<file_suffix>.csv        the end-of-run market report: before vs after, trades, resting and waiting orders, final book, price/book/portfolio time series (see market_recorder.hpp; plot with scripts/plot_market_report.py)
    // Empty output_dir = write no files at all. The suffix keeps simulation files ("_simu") apart from the socket server's ("_bots") when both runs share one folder.
    std::string output_dir;
    std::string file_suffix{"_simu"};
    bool write_metrics{true};
    bool write_report{true};
    std::chrono::milliseconds metrics_interval{1000};
    std::chrono::milliseconds report_interval{250};
};

class Simulation {
public:
    explicit Simulation(SimulationConfig config);

    // Runs the trading days, then stops everything and returns a human-readable report
    std::string run();

    MatchingEngine& engine() { return engine_; }
    const MarketData& market_data() const { return market_data_; }

private:
    void spawn_bots();
    void run_release_watcher(std::atomic<bool>& stop_flag);
    void run_metrics_recorder(std::atomic<bool>& stop_flag);
    void log_engine_events(); // subscribes a logger to the notification bus: trades, rejections, queued/expired orders
    void run_trading_days(TimePoint start); // the day loop, returns after the last close
    std::string trading_days_report() const;

    SimulationConfig config_;
    NotificationBus bus_;
    MatchingEngine engine_;
    MarketData market_data_;
    std::vector<std::unique_ptr<InProcessGateway>> gateways_; // one per bot, owned here, outlive the runners
    std::vector<std::unique_ptr<BotRunner>> bots_;
    std::unique_ptr<MarketFeed> feed_; // streams each day's bars to the policy server when RL bots run
    MarketSession session_;
    std::unique_ptr<FundamentalValues> fundamentals_;
    void sleep_trading(std::chrono::milliseconds duration, std::chrono::milliseconds day_length); // sleeps while the fundamental values diffuse
    std::unique_ptr<DailyBarBuilder> daily_bars_;
    int leaked_models_{0};             // agents refused: trained on a simulation with this run's seed
    void spawn_rl_bots(const std::vector<Symbol>& all_symbols, const std::unordered_map<Symbol, double>& volatilities, unsigned& seed);
    std::unordered_map<ClientId, double> initial_net_worth_;
    std::unordered_map<ClientId, Portfolio> initial_portfolio_; // the starting account: holding it unchanged is every bot's baseline
    ClientId next_client_id_{1};
    bool metrics_thread_started_{false};
    // declared last so it's destroyed first: it unsubscribes from bus_ in its destructor
    std::unique_ptr<MarketRecorder> recorder_;
};

} // namespace sim

#endif // SIMULATION_HPP