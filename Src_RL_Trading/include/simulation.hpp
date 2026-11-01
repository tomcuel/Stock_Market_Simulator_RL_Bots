#ifndef SIMULATION_HPP
#define SIMULATION_HPP

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "bot.hpp"
#include "market_data.hpp"
#include "market_recorder.hpp"
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
    int ml_traders{2};                                    // only started when the model below loads
    std::string ml_model_path{"models/signal_model.csv"}; // written by scripts/train_signal_model.py
        std::chrono::milliseconds ml_bar_interval{1000};      // the ML bots' bar: one simulated trading day (see MLSignalBot)
    // every bot starts with the same equity, bot_cash_fraction of it in cash and the rest split equally by value across the symbols (MatchingEngine::fund_balanced_account)
    // 10 million keeps whole shares fine-grained even for index-priced symbols (^DJI around 42000: 23 shares instead of 2)
    double bot_equity{10000000.0};
    double bot_cash_fraction{0.5};
    std::chrono::milliseconds bot_min_interval{20};
    std::chrono::milliseconds bot_max_interval{200};
    std::chrono::seconds duration{10};
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

    // Runs bots for `config.duration`, then stops everything and returns a human-readable report
    std::string run();

    MatchingEngine& engine() { return engine_; }
    const MarketData& market_data() const { return market_data_; }

private:
    void spawn_bots();
    void run_release_watcher(std::atomic<bool>& stop_flag);
    void run_metrics_recorder(std::atomic<bool>& stop_flag);
    void log_engine_events(); // subscribes a logger to the notification bus: trades, rejections, queued/expired orders

    SimulationConfig config_;
    NotificationBus bus_;
    MatchingEngine engine_;
    MarketData market_data_;
    std::vector<std::unique_ptr<InProcessGateway>> gateways_; // one per bot, owned here, outlive the runners
    std::vector<std::unique_ptr<BotRunner>> bots_;
    std::shared_ptr<const SignalModel> ml_model_;
    std::unordered_map<ClientId, double> initial_net_worth_;
    ClientId next_client_id_{1};
    bool metrics_thread_started_{false};
    // declared last so it's destroyed first: it unsubscribes from bus_ in its destructor
    std::unique_ptr<MarketRecorder> recorder_;
};

} // namespace sim

#endif // SIMULATION_HPP