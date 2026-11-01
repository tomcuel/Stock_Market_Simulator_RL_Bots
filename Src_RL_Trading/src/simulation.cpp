#include "simulation.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <map>
#include <random>
#include <sstream>

#include "logger.hpp"
#include "metrics.hpp"
#include "paths.hpp"

namespace sim {

Simulation::Simulation(SimulationConfig config) : config_(std::move(config)), engine_(bus_), market_data_(bus_) {
    for (const auto& symbol_config : config_.symbols) {
        engine_.register_symbol(symbol_config.symbol, symbol_config.initial_price);
    }
    config_.session.days = std::max(1, config_.session.days); // trading days are the only mode: at least one
    daily_bars_ = std::make_unique<DailyBarBuilder>(bus_);
    if (config_.value_investors > 0) {
        fundamentals_ = std::make_unique<FundamentalValues>(config_.seed * 31u + 5u, config_.fundamental_overnight_share, config_.fundamental_drift);
        for (const auto& symbol_config : config_.symbols) {
            fundamentals_->add(symbol_config.symbol, symbol_config.initial_price, symbol_config.volatility / std::sqrt(252.0));
        }
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
            auto bot = make_strategy(strategy, *gateways_.back(), all_symbols, seed++, volatilities);
            if (!bot) {
                continue;
            }
            if (auto* maker = dynamic_cast<MarketMakerBot*>(bot.get())) {
                maker->set_half_spread(config_.maker_half_spread);
            }
            bots_.push_back(std::make_unique<BotRunner>(std::move(bot), config_.bot_min_interval, config_.bot_max_interval, seed++));
            bots_.back()->set_session(&session_);
        }
    };

    spawn("noise", config_.noise_traders);
    spawn("momentum", config_.momentum_traders);
    spawn("marketmaker", config_.market_makers);
    spawn("meanreversion", config_.mean_reversion_traders);
    spawn("trend", config_.trend_followers);
    spawn("twap", config_.twap_executors);
    for (int i = 0; i < config_.value_investors && fundamentals_; ++i) {
        ClientId id = make_client();
        gateways_.push_back(std::make_unique<InProcessGateway>(engine_, id));
        auto bot = std::make_unique<ValueInvestorBot>(*gateways_.back(), all_symbols, seed++, volatilities, fundamentals_.get());
        bots_.push_back(std::make_unique<BotRunner>(std::move(bot), config_.bot_min_interval, config_.bot_max_interval, seed++));
        bots_.back()->set_session(&session_);
    }
    if (config_.rl_port > 0) {
        spawn_rl_bots(all_symbols, volatilities, seed);
    }
}

void Simulation::spawn_rl_bots(const std::vector<Symbol>& all_symbols, const std::unordered_map<Symbol, double>& volatilities, unsigned& seed) {
    // which agents the policy server serves, and what each one needs (lookback, training provenance)
    std::vector<PolicyInfo> served;
    PolicyClient probe;
    if (!probe.connect(config_.rl_host, config_.rl_port)) {
        LOG_WARN("no policy server on ", config_.rl_host, ":", config_.rl_port, ": no RL bots");
        return;
    }
    for (const auto& name : probe.models()) {
        auto info = probe.info(name);
        if (!info) {
            continue;
        }
        // never evaluate an agent on the market it learned from: an agent trained on a simulation with this very seed would
        // face the same random draws again (data leakage)
        if (info->trained_on_seed(static_cast<long long>(config_.seed))) {
            LOG_WARN("RL agent ", name, " was trained on a simulation with seed ", config_.seed, ", the seed of this run: no bots for it (use another --seed to evaluate it live)");
            ++leaked_models_;
            continue;
        }
        served.push_back(*info);
    }
    for (const auto& model : served) {
        if (model.agent != "ppo" && model.agent != "rule") { // ppo (learned) or rule (a baseline)
            LOG_WARN("RL agent ", model.name, " (", model.agent, ") is not an agent type the RL bots trade: no bots for it");
            continue;
        }
        LOG_INFO("RL agent ", model.name, ": ", config_.rl_bots_per_model, " bots");
        for (int i = 0; i < config_.rl_bots_per_model; ++i) {
            auto client = std::make_shared<PolicyClient>(); // one connection per bot, they run in parallel
            if (!client->connect(config_.rl_host, config_.rl_port)) {
                continue;
            }
            ClientId id = next_client_id_++;
            engine_.fund_balanced_account(id, config_.bot_equity, config_.bot_cash_fraction);
            gateways_.push_back(std::make_unique<InProcessGateway>(engine_, id));
            gateways_.back()->track_fills(); // its real fill prices: entries in the auction, released stops, targets
            auto bot = std::make_unique<RLBot>(*gateways_.back(), all_symbols, seed++, volatilities, client, model, config_.rl_risk_fraction);
            bot->set_session(&session_);
            bots_.push_back(std::make_unique<BotRunner>(std::move(bot), config_.bot_min_interval, config_.bot_max_interval, seed++));
            bots_.back()->set_session(&session_);
        }
    }
}

std::string Simulation::run() {
    LOG_INFO("simulation starting: ", bots_.size(), " bots trading across ", config_.symbols.size(), " symbols, ", config_.session.days, " trading days of ", config_.session.day.count(), " ms");

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
        recorder_config.seed = static_cast<long long>(config_.seed);
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
        initial_portfolio_[client] = engine_.portfolio_snapshot(client);
        initial_net_worth_[client] = initial_portfolio_[client].net_worth(initial_prices);
    }

    // the policy server's streams fill, one bar per day at every close, before the RL bots ask for their first trade
    if (config_.rl_port > 0) {
        feed_ = std::make_unique<MarketFeed>();
        if (feed_->connect(config_.rl_host, config_.rl_port)) {
            LOG_INFO("market feed: each day's bar (open, high, low, close, volume, the book before the pre-close) to the policy server at every close");
        } else {
            LOG_WARN("market feed: cannot reach the policy server on ", config_.rl_host, ":", config_.rl_port, ", the RL bots will wait for bars that never come");
            feed_.reset();
        }
    }

    for (auto& bot : bots_) {
        bot->start();
    }

    run_trading_days(start);

    for (auto& bot : bots_) {
        bot->stop();
    }
    if (feed_) {
        LOG_INFO("market feed: ", feed_->days_sent(), " days, ", feed_->bars_sent(), " daily bars streamed to the policy server");
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
        if (!daily_bars_->write_csv(config_.output_dir, config_.file_suffix)) {
            LOG_ERROR("cannot write the daily bars into ", config_.output_dir);
        }
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

    report << trading_days_report();
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
        double excess{0.0}; // P&L minus what holding the starting account unchanged made: what the strategy itself added
        std::uint64_t placed{0};
        std::uint64_t retracted{0};
    };
    std::map<std::string, StrategyScore> scores;
    auto final_prices = engine_.all_last_prices();
    for (auto& bot : bots_){
        TradingStrategy& strategy = bot->strategy();
        StrategyScore& score = scores[strategy.name()];
        ++score.bots;
        double final_worth = engine_.portfolio_snapshot(strategy.client()).net_worth(final_prices);
        score.pnl += final_worth - initial_net_worth_[strategy.client()];
        score.excess += final_worth - initial_portfolio_[strategy.client()].net_worth(final_prices);
        score.placed += strategy.orders_placed();
        score.retracted += strategy.orders_retracted();
    }
    auto score_line = [&report](const std::string& name, const StrategyScore& score) {
        report << name << ": bots=" << score.bots << " mean_pnl=" << score.pnl / score.bots << " mean_excess_vs_holding=" << score.excess / score.bots
               << " orders_placed=" << score.placed << " orders_retracted=" << score.retracted << "\n";
    };
    // the strategies that only see prices, ranked by what they added over holding their starting account. The value
    // investors are not among them: they see each company's hidden fundamental value (fundamentals.hpp), the variable that
    // moves the prices, so they are an informed benchmark (what knowing the value is worth), never a strategy to rank
    std::vector<std::pair<std::string, StrategyScore>> ranked;
    for (const auto& [name, score] : scores) {
        if (!is_informed_benchmark(name)) ranked.emplace_back(name, score);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {return a.second.excess / a.second.bots > b.second.excess / b.second.bots;});
    report << "\n-- Strategies (they only see prices; ranked by mean_excess_vs_holding) --\n";
    for (const auto& [name, score] : ranked) score_line(name, score);
    bool informed = false;
    for (const auto& [name, score] : scores) {
        if (!is_informed_benchmark(name)) continue;
        if (!informed) report << "\n-- Informed benchmark (sees the hidden fundamental value, not ranked with the strategies) --\n";
        informed = true;
        score_line(name, score);
    }
    // the RL bots, per agent: their trades as they really went (the walk-forward test windows judged them offline)
    struct RLScore {
        std::uint64_t bots = 0, days = 0, decisions = 0, entries = 0, waited = 0, failed = 0, rejected = 0, missed = 0;
        std::map<std::string, int> exits;
        double r_sum = 0.0;
        int wins = 0, closed = 0;
    };
    std::map<std::string, RLScore> rl_scores;
    std::vector<std::pair<std::string, RLTrade>> rl_trades;
    for (auto& bot : bots_) {
        if (auto* b = dynamic_cast<RLBot*>(&bot->strategy())) {
            auto& f = rl_scores[b->name()];
            ++f.bots;
            f.days = std::max<std::uint64_t>(f.days, b->days_seen());
            f.decisions += b->decisions();
            f.entries += b->entries();
            f.waited += b->waited();
            f.failed += b->failures();
            f.rejected += b->legs_rejected();
            f.missed += b->missed_opens();
            for (const auto& t : b->trades()) {
                ++f.exits[t.reason];
                f.r_sum += t.r();
                f.wins += t.r() > 0.0 ? 1 : 0;
                ++f.closed;
                rl_trades.emplace_back(std::string(b->name()) + "#" + std::to_string(b->client()), t);
            }
        }
    }
    for (const auto& [name, f] : rl_scores) {
        report << name << " rl: days=" << f.days << " decisions=" << f.decisions << " entries=" << f.entries << " closed=" << f.closed
               << " mean_r=" << (f.closed ? f.r_sum / f.closed : 0.0) << " win_rate=" << (f.closed ? static_cast<double>(f.wins) / f.closed : 0.0) << " exits:";
        for (const auto& [reason, n] : f.exits) report << " " << reason << "=" << n;
        report << " waited=" << f.waited << " failed=" << f.failed << " legs_rejected=" << f.rejected << " missed_opens=" << f.missed << "\n";
    }
    if (has_output && !rl_trades.empty()) {
        std::ofstream out(std::filesystem::path(config_.output_dir) / ("rl_trades" + config_.file_suffix + ".csv"));
        out << "bot,symbol,direction,entry_day,exit_day,quantity,entry,exit,stop,target,sl_atr,tp_r,reason,r\n" << std::setprecision(10);
        for (const auto& [bot, t] : rl_trades) {
            out << bot << ',' << t.symbol << ',' << t.direction << ',' << t.entry_day << ',' << t.exit_day << ',' << t.quantity << ',' << t.entry << ','
                << t.exit << ',' << t.stop << ',' << t.target << ',' << t.sl_atr << ',' << t.tp_r << ',' << t.reason << ',' << t.r() << '\n';
        }
    }
    if (leaked_models_ > 0) {
        report << "(" << leaked_models_ << " RL agent(s) skipped: trained on this very simulation seed)\n";
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

void Simulation::run_trading_days(TimePoint start) {
    const SessionConfig& s = config_.session;
    auto now_ms = [start] {return std::chrono::duration<double, std::milli>(Clock::now() - start).count();};
    std::mt19937 rng(config_.seed * 7919u + 17u); // the overnight news: its own stream, the same for every run with this seed
    std::normal_distribution<double> news(0.0, std::max(0.0, s.overnight_gap_vol));
    auto pre_open = std::min(s.pre_open, s.day / 3);
    auto pre_close = std::min(s.pre_close, s.day / 3);
    auto continuous = s.day - pre_open - pre_close;
    for (int day = 0; day < s.days; ++day) {
        // the open: overnight news moves the reference prices, the opening auction executes the carried GTC orders that cross
        std::unordered_map<Symbol, double> gaps;
        if (day > 0 && fundamentals_ && s.overnight_gap_vol <= 0.0) {
            gaps = fundamentals_->overnight(); // the night's news moves the values, and the price the market opens from
            for (const auto& [symbol, gap] : gaps) {
                engine_.gap_reference_price(symbol, std::exp(gap));
            }
        }
        else if (day > 0 && s.overnight_gap_vol > 0.0) {
            for (const auto& symbol : engine_.symbols()) {
                double gap = news(rng);
                engine_.gap_reference_price(symbol, std::exp(gap));
                gaps[symbol] = gap;
            }
        }
        engine_.open_market();
        daily_bars_->start_day(day, now_ms(), engine_.all_last_prices(), gaps);
        engine_.set_auction_mode(true);
        session_.set(day, SessionPhase::PRE_OPEN);
        if (recorder_) recorder_->record_phase(day, "PRE_OPEN");
        sleep_trading(pre_open, s.day);
        engine_.run_opening_auction(); // the open: everything collected (and the GTC orders it crosses) at one price per symbol
        engine_.expire_market_orders();
        session_.set(day, SessionPhase::OPEN);
        if (recorder_) recorder_->record_phase(day, "OPEN");
        sleep_trading(continuous, s.day);

        // the pre-close: the book as it stands, then orders collected without matching
        for (const auto& symbol : engine_.symbols()) {
            auto snap = engine_.snapshot(symbol, 10);
            double bid_depth = 0.0, ask_depth = 0.0;
            for (const auto& level : snap.bids) bid_depth += static_cast<double>(level.quantity);
            for (const auto& level : snap.asks) ask_depth += static_cast<double>(level.quantity);
            daily_bars_->set_book(symbol, snap.bids.empty() ? 0.0 : snap.bids.front().price, snap.asks.empty() ? 0.0 : snap.asks.front().price, bid_depth, ask_depth);
        }
        engine_.set_auction_mode(true);
        session_.set(day, SessionPhase::PRE_CLOSE);
        if (recorder_) recorder_->record_phase(day, "PRE_CLOSE");
        sleep_trading(pre_close, s.day);

        // the close: the closing auction, DAY orders expire, the day's bar goes to the policy server
        engine_.close_market();
        session_.set(day, SessionPhase::CLOSED);
        if (recorder_) recorder_->record_phase(day, "CLOSED");
        std::unordered_map<Symbol, Price> closes;
        std::unordered_map<Symbol, Quantity> volumes;
        for (const auto& close : engine_.run_closing_auction()) {
            closes[close.symbol] = close.price;
            volumes[close.symbol] = close.volume;
        }
        engine_.expire_day_orders(); // MARKET orders left in the closing auction included
        bus_.flush(); // every trade of the day (the auction's included) reaches the bar
        auto bars = daily_bars_->finish_day(now_ms(), closes, volumes);
        if (feed_) {
            feed_->push_bars(bars);
        }
        std::this_thread::sleep_for(s.overnight);
    }
}

void Simulation::sleep_trading(std::chrono::milliseconds duration, std::chrono::milliseconds day_length) {
    if (!fundamentals_) {
        std::this_thread::sleep_for(duration);
        return;
    }
    constexpr auto kStep = std::chrono::milliseconds(50);
    auto end = Clock::now() + duration;
    double day_ms = static_cast<double>(std::max<std::int64_t>(1, day_length.count()));
    while (Clock::now() < end) {
        auto step = std::min<Clock::duration>(kStep, end - Clock::now());
        std::this_thread::sleep_for(step);
        fundamentals_->diffuse(std::chrono::duration<double, std::milli>(step).count() / day_ms);
    }
}

// the shape of the daily series, per symbol: what a real market's daily closes would be judged on
std::string Simulation::trading_days_report() const {
    std::map<Symbol, std::vector<DailyBar>> by_symbol;
    for (const auto& bar : daily_bars_->all()) {
        by_symbol[bar.symbol].push_back(bar);
    }
    std::ostringstream out;
    out << "\n-- Trading days (" << config_.session.days << " days of " << config_.session.day.count() << " ms, pre-open " << config_.session.pre_open.count() << " ms, pre-close " << config_.session.pre_close.count()
        << " ms, overnight " << config_.session.overnight.count() << " ms, gap vol " << config_.session.overnight_gap_vol << ") --\n";
    for (const auto& [symbol, bars] : by_symbol) {
        std::vector<double> r;
        double gap_abs = 0.0, range = 0.0, intraday = 0.0;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            if (i > 0 && bars[i - 1].close > 0.0 && bars[i].close > 0.0) r.push_back(std::log(bars[i].close / bars[i - 1].close));
            gap_abs += std::abs(bars[i].gap);
            if (bars[i].close > 0.0) range += (bars[i].high - bars[i].low) / bars[i].close;
            if (bars[i].open > 0.0 && bars[i].close > 0.0) intraday += std::log(bars[i].close / bars[i].open);
        }
        double n = static_cast<double>(std::max<std::size_t>(1, r.size()));
        double mean = 0.0;
        for (double x : r) mean += x;
        mean /= n;
        double var = 0.0, cov = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            var += (r[i] - mean) * (r[i] - mean);
            if (i > 0) cov += (r[i] - mean) * (r[i - 1] - mean);
        }
        double days = static_cast<double>(std::max<std::size_t>(1, bars.size()));
        out << symbol << ": days=" << bars.size() << " first_close=" << (bars.empty() ? 0.0 : bars.front().close) << " last_close=" << (bars.empty() ? 0.0 : bars.back().close)
            << " mean_daily_return_bp=" << 1e4 * mean << " daily_vol_bp=" << 1e4 * std::sqrt(var / n) << " autocorr_1=" << (var > 0.0 ? cov / var : 0.0)
            << " mean_range_bp=" << 1e4 * range / days << " mean_open_to_close_bp=" << 1e4 * intraday / days << " mean_abs_gap_bp=" << 1e4 * gap_abs / days << "\n";
    }
    return out.str();
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
