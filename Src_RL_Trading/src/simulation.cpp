#include "simulation.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "logger.hpp"
#include "metrics.hpp"
#include "paths.hpp"

namespace sim {

Simulation::Simulation(SimulationConfig config) : config_(std::move(config)), engine_(bus_), market_data_(bus_) {
    for (const auto& symbol_config : config_.symbols) {
        engine_.register_symbol(symbol_config.symbol, symbol_config.initial_price);
    }
    log_engine_events();
    spawn_bots();
}

// Subscribes a logger to every engine event so trades and the server's/simulation's interactions with orders are actually visible
// previously nothing consumed these NotificationBus events at all beyond the watcher thread's own summary line, so a trade could execute and leave no trace in the log whatsoever
void Simulation::log_engine_events() {
    bus_.subscribe([](const Event& event) {
        if (auto* trade = std::get_if<TradeEvent>(&event)) {
            LOG_INFO("TRADE ", trade->trade.symbol, " qty=", trade->trade.quantity, " price=", trade->trade.price, " buyer=", trade->trade.buyer, " seller=", trade->trade.seller);
        } 
        else if (auto* rejected = std::get_if<OrderRejectedEvent>(&event)) {
            LOG_WARN("ORDER_REJECTED order_id=", rejected->order_id, " client=", rejected->client, " reason=", rejected->reason);
        } 
        else if (auto* queued = std::get_if<OrderQueuedEvent>(&event)) {
            LOG_INFO("ORDER_QUEUED order_id=", queued->order_id, " client=", queued->client, " symbol=", queued->symbol);
        } 
        else if (auto* expired = std::get_if<OrderExpiredEvent>(&event)) {
            LOG_INFO("ORDER_EXPIRED order_id=", expired->order_id, " client=", expired->client, " symbol=", expired->symbol);
        }
        // OrderAcceptedEvent isn't logged separately: it fires for every accepted order (including ones that immediately produced the TradeEvent(s) already logged above), so logging both would mostly just double the noise for filled orders. 
        // STOP/LIMIT/LIMIT_STOP orders that rest without matching anything are still visible via ORDER_QUEUED or the client's own "status=RESTING" response, at the server layer
    });
}

void Simulation::spawn_bots() {
    std::vector<Symbol> all_symbols;
    std::unordered_map<Symbol, double> volatilities;
    for (const auto& symbol_config : config_.symbols) {
        all_symbols.push_back(symbol_config.symbol);
        volatilities[symbol_config.symbol] = symbol_config.volatility;
    }

    unsigned seed = config_.seed;

    auto make_client = [&]() {
        ClientId id = next_client_id_++;
        // balanced by value: the same equity for every bot, split between cash and an equal value of every symbol, so a bot can both buy and sell whichever symbol pick_symbol() hands it
        engine_.fund_balanced_account(id, config_.bot_equity, config_.bot_cash_fraction);
        return id;
    };

    // one entry per strategy: how many bots run it
    auto spawn = [&](const std::string& strategy, int count) {
        for (int i = 0; i < count; ++i) {
            ClientId id = make_client();
            gateways_.push_back(std::make_unique<InProcessGateway>(engine_, id));
            auto bot = make_strategy(strategy, *gateways_.back(), all_symbols, seed++, volatilities, ml_model_, config_.ml_bar_interval);
            if (!bot) {
                continue;
            }
            bots_.push_back(std::make_unique<BotRunner>(std::move(bot), config_.bot_min_interval, config_.bot_max_interval, seed++));
        }
    };

    int ml_count = config_.ml_traders;
    if (ml_count > 0) {
        auto model = std::make_shared<SignalModel>();
        std::string error;
        if (model->load(config_.ml_model_path, error)) {
            ml_model_ = model;
            LOG_INFO("ML model loaded from ", display_path(config_.ml_model_path), " (horizon ", model->horizon(), " bars of ", config_.ml_bar_interval.count(), " ms)");
            // each ML bot needs kSignalMinHistory bars before its first decision, then horizon() bars per position: a run shorter than that plus a few horizons gives too few trades to judge the model
            if (config_.ml_bar_interval.count() > 0) {
                auto bars = static_cast<long long>(config_.duration / config_.ml_bar_interval);
                auto useful = static_cast<long long>(kSignalMinHistory) + 4LL * std::max(1, model->horizon());
                if (bars < useful) {
                    LOG_WARN("this run lasts ", bars, " ML bars (simulated trading days): the ML bots need ", kSignalMinHistory, " to start and ", model->horizon(), " per position, run at least ", useful * config_.ml_bar_interval.count() / 1000, " s (--duration) for their results to mean anything");
                }
            }
        }
        else {
            LOG_WARN("no ML bots: ", error);
            ml_count = 0;
        }
    }

    spawn("noise", config_.noise_traders);
    spawn("momentum", config_.momentum_traders);
    spawn("marketmaker", config_.market_makers);
    spawn("meanreversion", config_.mean_reversion_traders);
    spawn("trend", config_.trend_followers);
    spawn("twap", config_.twap_executors);
    spawn("ml", ml_count);
}

std::string Simulation::run() {
    LOG_INFO("simulation starting: ", bots_.size(), " bots trading across ", config_.symbols.size(), " symbols, running for ", config_.duration.count(), "s");

    MetricsRegistry::instance().reset();
    auto start = std::chrono::steady_clock::now();

    std::atomic<bool> stop_watcher{false};
    std::thread watcher([this, &stop_watcher] {run_release_watcher(stop_watcher);});

    const bool has_output = !config_.output_dir.empty();
    if (has_output) {
        std::error_code ec;
        std::filesystem::create_directories(config_.output_dir, ec);
        if (ec) {
            LOG_ERROR("cannot create output directory ", config_.output_dir, ": ", ec.message());
        }
    }

    std::atomic<bool> stop_metrics{false};
    std::thread metrics_thread;
    metrics_thread_started_ = false;
    if (has_output && config_.write_metrics) {
        metrics_thread_started_ = true;
        metrics_thread = std::thread([this, &stop_metrics] {run_metrics_recorder(stop_metrics);});
    }

    if (has_output && config_.write_report) {
        RecorderConfig recorder_config;
        recorder_config.output_dir = config_.output_dir;
        recorder_config.file_suffix = config_.file_suffix;
        recorder_config.sample_interval = config_.report_interval;
        recorder_ = std::make_unique<MarketRecorder>(bus_, engine_, recorder_config);
        for (auto& bot : bots_) {
            recorder_->set_client_label(bot->strategy().client(), bot->strategy().name()); // P&L per strategy in the report
        }
        recorder_->start(); // "before" snapshot (initial prices, every bot's starting portfolio) and the sampling thread: without it the report only holds the final snapshot
    }

    // net worth at the start, valued at the initial prices: the base of each strategy's P&L
    auto initial_prices = engine_.all_last_prices();
    for (auto& bot : bots_){
        ClientId client = bot->strategy().client();
        initial_net_worth_[client] = engine_.portfolio_snapshot(client).net_worth(initial_prices);
    }

    for (auto& bot : bots_) {
        bot->start();
    }

    std::this_thread::sleep_for(config_.duration);

    for (auto& bot : bots_) {
        bot->stop();
    }

    stop_watcher.store(true);
    watcher.join();
    if (metrics_thread.joinable()) {
        stop_metrics.store(true);
        metrics_thread.join();
    }

    bool market_report_written = false;
    if (recorder_) {
        recorder_->stop();
        market_report_written = recorder_->write_report(); // "after" snapshot, taken once everything has stopped
    }

    double elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::ostringstream report;
    report << "=== Simulation report (" << elapsed_seconds << "s) ===\n";
    if (has_output) {
        report << "output: " << display_path(config_.output_dir) << '\n';
        if (metrics_thread_started_) {
            report << "  metrics" << config_.file_suffix << ".csv  (plot: python3 scripts/plot_metrics.py " << display_path(config_.output_dir) << ")\n";
        }
        if (recorder_) {
            report << (market_report_written ? "  market report *" + config_.file_suffix + ".csv  (plot: python3 scripts/plot_market_report.py " + display_path(config_.output_dir) + ")\n" : std::string("  market report: FAILED to write (see log)\n"));
        }
    }
    report << MetricsRegistry::instance().report(elapsed_seconds) << "\n";
    report << "waiting_orders=" << engine_.waiting_order_count() << " (STOP/LIMIT_STOP orders still waiting for their release band, or a future start date)\n";

    report << "\n-- Symbols --\n";
    for (const auto& symbol_config : config_.symbols) {
        const auto& symbol = symbol_config.symbol;
        report << symbol << ": last=" << market_data_.last_price(symbol) << " vwap=" << market_data_.vwap(symbol) << " volume=" << market_data_.total_volume(symbol);
        auto snap = engine_.snapshot(symbol, 1);
        if (!snap.bids.empty()) {
            report << " best_bid=" << snap.bids.front().price;
        }
        if (!snap.asks.empty()) {
            report << " best_ask=" << snap.asks.front().price;
        }
        report << "\n";
    }

    // per strategy: how the bots running it did, and how much they managed their own orders
    struct StrategyScore
    {
        int bots{0};
        double pnl{0.0};
        std::uint64_t placed{0};
        std::uint64_t retracted{0};
    };
    std::map<std::string, StrategyScore> scores;
    auto final_prices = engine_.all_last_prices();
    for (auto& bot : bots_){
        TradingStrategy& strategy = bot->strategy();
        StrategyScore& score = scores[strategy.name()];
        ++score.bots;
        score.pnl += engine_.portfolio_snapshot(strategy.client()).net_worth(final_prices) - initial_net_worth_[strategy.client()];
        score.placed += strategy.orders_placed();
        score.retracted += strategy.orders_retracted();
    }
    report << "\n-- Strategies --\n";
    for (const auto& [name, score] : scores){
        report << name << ": bots=" << score.bots << " mean_pnl=" << score.pnl / score.bots << " orders_placed=" << score.placed << " orders_retracted=" << score.retracted << "\n";
    }
    // the ML bots' positions: how they ended (closed at the model's horizon, or earlier by their protective stop)
    std::uint64_t ml_bars = 0, ml_opened = 0, ml_horizon = 0, ml_stop = 0, ml_open = 0, ml_bots = 0;
    for (auto& bot : bots_) {
        if (auto* ml = dynamic_cast<MLSignalBot*>(&bot->strategy())) {
            ++ml_bots;
            ml_bars = std::max(ml_bars, ml->bars_seen());
            ml_opened += ml->positions_opened();
            ml_horizon += ml->horizon_exits();
            ml_stop += ml->stop_exits();
            ml_open += ml->open_positions();
        }
    }
    if (ml_bots > 0) {
        report << "ml positions: bars=" << ml_bars << " opened=" << ml_opened << " closed_at_horizon=" << ml_horizon << " closed_by_stop=" << ml_stop << " still_open=" << ml_open << "\n";
    }

    report << "\n-- Top 5 clients by net worth --\n";
    auto last_prices = engine_.all_last_prices();
    std::vector<std::pair<ClientId, double>> leaderboard;
    for (ClientId id = 1; id < next_client_id_; ++id) {
        Portfolio p = engine_.portfolio_snapshot(id);
        leaderboard.emplace_back(id, p.net_worth(last_prices));
    }
    std::sort(leaderboard.begin(), leaderboard.end(), [](auto& a, auto& b) {return a.second > b.second;});
    for (std::size_t i = 0; i < std::min<std::size_t>(5, leaderboard.size()); ++i) {
        report << "client " << leaderboard[i].first << ": net_worth=" << leaderboard[i].second << "\n";
    }

    LOG_INFO("simulation finished");
    return report.str();
}

// periodically promotes waiting STOP/LIMIT_STOP orders into the live book once their release band and start date are satisfied, and drops expired ones
// the in-process equivalent of SimulationServer's background watcher thread (see net/simulation_server.cpp) and, before that, Src_SQL's trigger_and_expiration_watcher
void Simulation::run_release_watcher(std::atomic<bool>& stop_flag) {
    int tick = 0;
    while (!stop_flag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++tick;
        engine_.try_release_waiting_orders(/*full_scan=*/tick % 10 == 0);
    }
    engine_.try_release_waiting_orders(/*full_scan=*/true); // one last, full pass so the final report is accurate
}

// Appends one CSV row of throughput/latency metrics every metrics_interval, so a run can be plotted after the fact instead of only ever being visible as one final summary line
void Simulation::run_metrics_recorder(std::atomic<bool>& stop_flag) {
    std::string path = (std::filesystem::path(config_.output_dir) / ("metrics" + config_.file_suffix + ".csv")).string();
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR("metrics recorder: failed to open ", path, " for writing");
        return;
    }
    out << "elapsed_ms,orders_submitted,orders_accepted,orders_rejected,orders_queued,orders_expired,trades_executed,volume_traded,waiting_orders,mean_latency_us,max_latency_us\n";

    auto start = std::chrono::steady_clock::now();
    auto write_row = [&] {
        auto& metrics = MetricsRegistry::instance();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        out << elapsed_ms << ',' << metrics.orders_submitted() << ',' << metrics.orders_accepted() << ',' << metrics.orders_rejected() << ',' << metrics.orders_queued() << ',' << metrics.orders_expired() << ',' << metrics.trades_executed() << ',' << metrics.volume_traded() << ',' << engine_.waiting_order_count() << ',' << metrics.submit_latency().mean_us() << ',' << metrics.submit_latency().max_us() << '\n';
        out.flush();
    };
    while (!stop_flag.load()) {
        std::this_thread::sleep_for(config_.metrics_interval);
        if (stop_flag.load()) break;
        write_row();
    }
    write_row(); // final row, so the series always ends at the end of the run
}

} // namespace sim
