//=======================================================================
// Records what happened to the market during a run and writes an end-of-run report as plain CSV files (used for reporting)
//
// While running, it:
//   - captures every trade (via a NotificationBus subscription) plus rejection/queue/expiry counts
//   - samples every symbol's last price, best bid/ask and top-of-book depth every sample_interval (for price paths, spread, and order book pressure over time)
//   - samples every client's cash and net worth at the same interval (portfolio evolution)
// At the end, write_report() adds a final snapshot: every order still resting in the book, every order still waiting in the STOP/LIMIT_STOP/start-date registry, the full final order book depth, and before-vs-after figures per symbol and per client
//
// Files written to RecorderConfig::output_dir, each name ending in RecorderConfig::file_suffix (simulation.x uses "_simu", sim_server.x uses "_bots", so both runs can share one folder):
//   summary<sfx>.csv            key,value run-level totals
//   symbols<sfx>.csv            per symbol: initial vs final price, high/low/VWAP, volume, spread, depth, imbalance
//   trades<sfx>.csv             every executed trade (the "completed" side of the order lifecycle)
//   price_samples<sfx>.csv      time series: last price, best bid/ask, bid/ask depth per symbol
//   portfolio_samples<sfx>.csv  time series: cash and net worth per client
//   portfolios_final<sfx>.csv   per client: before vs after cash/net worth, P&L, trade counts, holdings
//   order_book_final<sfx>.csv   final book, one row per price level (quantity and number of orders)
//   resting_orders<sfx>.csv     every individual order still resting in the book at the end
//   waiting_orders<sfx>.csv     every order still waiting for its release band / start date at the end
//   rejections<sfx>.csv         rejection count per reason
//=======================================================================
#ifndef MARKET_RECORDER_HPP
#define MARKET_RECORDER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "matching_engine.hpp"
#include "notification.hpp"
#include "types.hpp"

namespace sim {

struct RecorderConfig {
    std::string output_dir;                               // created if missing
    std::string file_suffix;                              // appended to every file name, e.g. "_simu" -> trades_simu.csv
    std::chrono::milliseconds sample_interval{250};       // price/book/portfolio sampling period
    std::size_t book_depth{10};                           // price levels summed for bid/ask depth samples
};

class MarketRecorder {
public:
    MarketRecorder(NotificationBus& bus, const MatchingEngine& engine, RecorderConfig config);
    ~MarketRecorder();

    MarketRecorder(const MarketRecorder&) = delete;
    MarketRecorder& operator=(const MarketRecorder&) = delete;

    // Captures the "before" state (initial prices, every existing client's cash/net worth), subscribes to trade events and starts the background sampler. Call once, before trading starts
    void start();

    // Stops the background sampler (idempotent): Event capture keeps running until unsubscribed, so trades still in flight when this is called are not lost (write_report() flushes them)
    void stop();

    // Waits for every already-published event to be delivered, takes one final sample, and writes all report files
    // Returns false (and logs why) if the output directory or a file can't be written: Safe to call after stop()
    bool write_report();

    std::size_t recorded_trade_count() const;

    // a short text shown next to client in portfolios_final‹stx>.csv (the "label" column): the strategy name for simulation.x bots, the account username for sim_server.x clients
    void set_client_label(ClientId client, std::string label); 
    const std::string& output_dir() const {return config_.output_dir;}

private:
    struct TradeRow {
        double elapsed_ms;
        Trade trade;
    };
    struct PriceSample {
        double elapsed_ms;
        Symbol symbol;
        Price last_price;
        Price best_bid;   // 0 when the side is empty
        Price best_ask;   // 0 when the side is empty
        Quantity bid_depth;
        Quantity ask_depth;
    };
    struct PortfolioSample {
        double elapsed_ms;
        ClientId client;
        double cash;
        double reserved_cash;
        double net_worth;
    };
    struct FirstSeen {
        double elapsed_ms;
        double cash;
        double net_worth;
    };

    void on_event(const Event& event);
    void sampler_loop();
    void take_sample();
    void unsubscribe();
    double elapsed_ms(TimePoint t) const;

    NotificationBus& bus_;
    const MatchingEngine& engine_;
    RecorderConfig config_;

    std::mutex subscription_mutex_;
    bool subscribed_{false};
    SubscriptionId subscription_id_{0};

    TimePoint start_time_{};

    mutable std::mutex mutex_; // guards every recorded collection below
    std::vector<TradeRow> trades_;
    std::map<std::string, std::uint64_t> rejections_by_reason_;
    std::uint64_t queued_events_{0};
    std::uint64_t expired_events_{0};
    std::vector<PriceSample> price_samples_;
    std::vector<PortfolioSample> portfolio_samples_;
    std::unordered_map<Symbol, Price> initial_prices_;
    std::unordered_map<ClientId, FirstSeen> first_seen_; // a client's "before" state = first time it was sampled
    std::unordered_map<ClientId, std::string> labels_;
    
    std::atomic<bool> running_{false};
    std::mutex sampler_mutex_;
    std::condition_variable sampler_cv_; // lets stop() wake the sampler immediately instead of waiting a full interval
    std::thread sampler_;
};

} // namespace sim

#endif // MARKET_RECORDER_HPP