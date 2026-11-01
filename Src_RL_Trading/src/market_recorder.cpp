#include "market_recorder.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>

#include "logger.hpp"
#include "metrics.hpp"
#include "paths.hpp"

namespace sim {

namespace {

// writes a price bound, leaving the cell empty for an unbounded (+/-infinity) side so the CSV stays trivially parseable (pandas reads an empty cell as NaN)
void write_bound(std::ostream& out, Price value) {
    if (std::isfinite(value)) {
        out << value;
    }
}

// "SYM:qty;SYM:qty": keeps portfolios_final.csv one row per client regardless of symbol count
std::string holdings_to_string(const Portfolio& portfolio) {
    std::vector<std::pair<Symbol, Quantity>> sorted(portfolio.holdings.begin(), portfolio.holdings.end());
    std::sort(sorted.begin(), sorted.end());
    std::string result;
    for (const auto& [symbol, quantity] : sorted) {
        if (quantity == 0) {
            continue;
        }
        if (!result.empty()) {
            result += ';';
        }
        result += symbol + ':' + std::to_string(quantity);
    }
    return result;
}

// "SYM:qty;SYM:qty" of the shares reserved by open SELL orders
std::string reserved_to_string(const Portfolio& portfolio){
    std::vector<std::pair<Symbol, Quantity>> sorted(portfolio.reserved_shares.begin(), portfolio.reserved_shares.end());
    std::sort(sorted.begin(), sorted.end());
    std::string result;
    for (const auto& [symbol, quantity] : sorted){
        if (quantity == 0) {
            continue;
        }
        if (!result.empty()) {
            result += ';';
        }
        result += symbol + ':' + std::to_string(quantity);
    }
    return result;
}

} // namespace

MarketRecorder::MarketRecorder(NotificationBus& bus, const MatchingEngine& engine, RecorderConfig config) : bus_(bus), engine_(engine), config_(std::move(config)), start_time_(Clock::now()) {}

MarketRecorder::~MarketRecorder() {
    stop();
    unsubscribe(); // must happen before any member below is destroyed
}

double MarketRecorder::elapsed_ms(TimePoint t) const {
    return std::chrono::duration<double, std::milli>(t - start_time_).count();
}

void MarketRecorder::start() {
    if (running_.load()) {
        return;
    }
    start_time_ = Clock::now();

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        if (!subscribed_) {
            subscription_id_ = bus_.subscribe([this](const Event& event) {on_event(event);});
            subscribed_ = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        initial_prices_ = engine_.all_last_prices();
    }
    take_sample(); // the "before" snapshot: every client that already exists is first seen here

    running_.store(true);
    sampler_ = std::thread([this] {sampler_loop();});
}

void MarketRecorder::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    sampler_cv_.notify_all();
    if (sampler_.joinable()) {
        sampler_.join();
    }
}

void MarketRecorder::unsubscribe() {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    if (subscribed_) {
        bus_.unsubscribe(subscription_id_);
        subscribed_ = false;
    }
}

void MarketRecorder::set_client_label(ClientId client, std::string label)
{
    std::lock_guard<std::mutex> lock(mutex_);
    labels_[client] = std::move(label);
}

std::size_t MarketRecorder::recorded_trade_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trades_.size();
}

void MarketRecorder::on_event(const Event& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* trade = std::get_if<TradeEvent>(&event)) {
        trades_.push_back(TradeRow{elapsed_ms(trade->trade.timestamp), trade->trade});
    } 
    else if (auto* rejected = std::get_if<OrderRejectedEvent>(&event)) {
        ++rejections_by_reason_[rejected->reason];
    } 
    else if (std::get_if<OrderQueuedEvent>(&event)) {
        ++queued_events_;
    } 
    else if (std::get_if<OrderExpiredEvent>(&event)) {
        ++expired_events_;
    }
}

void MarketRecorder::sampler_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(sampler_mutex_);
        sampler_cv_.wait_for(lock, config_.sample_interval, [this] {return !running_.load();});
        lock.unlock();
        if (!running_.load()) {
            break;
        }
        take_sample();
    }
}

void MarketRecorder::take_sample() {
    // read everything from the engine first (each accessor takes its own engine-side lock), then  append under our own mutex_ (never hold mutex_ while calling into the engine)
    double t = elapsed_ms(Clock::now());
    auto prices = engine_.all_last_prices();
    auto portfolios = engine_.all_portfolios();

    std::vector<PriceSample> new_price_samples;
    for (const auto& symbol : engine_.symbols()) {
        auto snap = engine_.snapshot(symbol, config_.book_depth);
        Quantity bid_depth = 0, ask_depth = 0;
        for (const auto& level : snap.bids) {
            bid_depth += level.quantity;
        }
        for (const auto& level : snap.asks) {
            ask_depth += level.quantity;
        }
        auto price_it = prices.find(symbol);
        new_price_samples.push_back(PriceSample{
            t, symbol, price_it == prices.end() ? 0.0 : price_it->second,
            snap.bids.empty() ? 0.0 : snap.bids.front().price,
            snap.asks.empty() ? 0.0 : snap.asks.front().price,
            bid_depth, ask_depth});
    }

    std::lock_guard<std::mutex> lock(mutex_);
    price_samples_.insert(price_samples_.end(), new_price_samples.begin(), new_price_samples.end());
    for (const auto& [client, portfolio] : portfolios) {
        double net_worth = portfolio.net_worth(prices);
        portfolio_samples_.push_back(PortfolioSample{t, client, portfolio.cash, portfolio.reserved_cash, net_worth});
        first_seen_.try_emplace(client, FirstSeen{t, portfolio.cash, net_worth});
    }
}

bool MarketRecorder::write_report() {
    namespace fs = std::filesystem;

    bus_.flush();  // every trade already executed must be in trades_ before we write anything
    take_sample(); // the "after" state, so every time series ends exactly at the final snapshot

    std::error_code ec;
    fs::create_directories(config_.output_dir, ec);
    if (ec) {
        LOG_ERROR("market report: cannot create ", display_path(config_.output_dir), ": ", ec.message());
        return false;
    }

    // engine-side final state, read before taking our own lock
    auto final_prices = engine_.all_last_prices();
    auto final_portfolios = engine_.all_portfolios();
    auto symbols = engine_.symbols();
    std::sort(symbols.begin(), symbols.end());
    std::unordered_map<Symbol, std::vector<Order>> resting_by_symbol;
    std::size_t total_resting = 0;
    for (const auto& symbol : symbols) {
        resting_by_symbol[symbol] = engine_.resting_orders(symbol);
        total_resting += resting_by_symbol[symbol].size();
    }
    auto waiting = engine_.waiting_orders();
    std::sort(waiting.begin(), waiting.end(), [](const Order& a, const Order& b) {return a.id < b.id;});

    std::lock_guard<std::mutex> lock(mutex_);

    bool ok = true;
    // `base` is the file's name without extension, e.g. "trades" -> trades<file_suffix>.csv
    auto open_csv = [&](const std::string& base) {
        std::string name = base + config_.file_suffix + ".csv";
        std::ofstream out(fs::path(config_.output_dir) / name, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            LOG_ERROR("market report: cannot write ", name, " in ", display_path(config_.output_dir));
            ok = false;
        }
        out << std::setprecision(12);
        return out;
    };

    // per-symbol aggregates from the recorded trades 
    struct SymbolStats {
        std::uint64_t trades{0};
        Quantity volume{0};
        double notional{0.0};
        Price high{-std::numeric_limits<Price>::infinity()};
        Price low{std::numeric_limits<Price>::infinity()};
    };
    std::unordered_map<Symbol, SymbolStats> stats;
    std::unordered_map<ClientId, std::uint64_t> buys_by_client, sells_by_client;
    double total_notional = 0.0;
    Quantity total_volume = 0;
    for (const auto& row : trades_) {
        const Trade& tr = row.trade;
        auto& s = stats[tr.symbol];
        ++s.trades;
        s.volume += tr.quantity;
        s.notional += static_cast<double>(tr.quantity) * tr.price;
        s.high = std::max(s.high, tr.price);
        s.low = std::min(s.low, tr.price);
        ++buys_by_client[tr.buyer];
        ++sells_by_client[tr.seller];
        total_volume += tr.quantity;
        total_notional += static_cast<double>(tr.quantity) * tr.price;
    }
    std::unordered_map<Symbol, std::size_t> waiting_by_symbol;
    for (const auto& order : waiting) {
        ++waiting_by_symbol[order.symbol];
    }

    // trades.csv 
    {
        auto out = open_csv("trades");
        out << "elapsed_ms,symbol,quantity,price,buyer,seller,buy_order_id,sell_order_id\n";
        for (const auto& row : trades_) {
            const Trade& tr = row.trade;
            out << row.elapsed_ms << "," << tr.symbol << "," << tr.quantity << "," << tr.price << "," << tr.buyer << "," << tr.seller << "," << tr.buy_order_id << "," << tr.sell_order_id << "\n";
        }
    }

    // price_samples.csv 
    {
        auto out = open_csv("price_samples");
        out << "elapsed_ms,symbol,last_price,best_bid,best_ask,bid_depth,ask_depth\n";
        for (const auto& s : price_samples_) {
            out << s.elapsed_ms << "," << s.symbol << "," << s.last_price << "," << s.best_bid << "," << s.best_ask << "," << s.bid_depth << "," << s.ask_depth << "\n";
        }
    }

    // portfolio_samples.csv 
    {
        auto out = open_csv("portfolio_samples");
        out << "elapsed_ms,client,cash,reserved_cash,net_worth\n";
        for (const auto& s : portfolio_samples_) {
            out << s.elapsed_ms << "," << s.client << "," << s.cash << "," << s.reserved_cash << "," << s.net_worth << "\n";
        }
    }

    // order_book_final.csv + resting_orders.csv 
    {
        auto book_out = open_csv("order_book_final");
        auto orders_out = open_csv("resting_orders");
        book_out << "symbol,side,price,quantity,orders\n";
        orders_out << "order_id,client,symbol,side,kind,quantity,price\n";
        for (const auto& symbol : symbols) {
            // aggregate per (side, price): std::map keeps price levels sorted for readability
            std::map<Price, std::pair<Quantity, std::size_t>> bid_levels, ask_levels;
            for (const auto& order : resting_by_symbol[symbol]) {
                auto& levels = (order.side == Side::BUY) ? bid_levels : ask_levels;
                auto& level = levels[order.price];
                level.first += order.quantity;
                ++level.second;
                orders_out << order.id << "," << order.client << "," << symbol << "," << sim::to_string(order.side) << "," << sim::to_string(order.kind) << "," << order.quantity << "," << order.price << "\n";
            }
            for (auto it = bid_levels.rbegin(); it != bid_levels.rend(); ++it) {
                book_out << symbol << ",BUY," << it->first << "," << it->second.first << "," << it->second.second << "\n";
            }
            for (const auto& [price, level] : ask_levels) {
                book_out << symbol << ",SELL," << price << "," << level.first << "," << level.second << "\n";
            }
        }
    }

    // waiting_orders.csv 
    {
        auto out = open_csv("waiting_orders");
        out << "order_id,client,symbol,side,kind,quantity,price,release_lower,release_upper,has_start_date,has_expiry\n";
        for (const auto& order : waiting) {
            out << order.id << "," << order.client << "," << order.symbol << "," << sim::to_string(order.side) << "," << sim::to_string(order.kind) << "," << order.quantity << "," << order.price << ",";
            write_bound(out, order.release_lower);
            out << ",";
            write_bound(out, order.release_upper);
            out << "," << (order.not_before.has_value() ? 1 : 0) << "," << (order.expires_at.has_value() ? 1 : 0) << "\n";
        }
    }

    // symbols.csv (before vs after) 
    {
        auto out = open_csv("symbols");
        out << "symbol,initial_price,final_price,change_pct,high,low,vwap,trades,volume,notional,best_bid,best_ask,spread,bid_depth,ask_depth,imbalance,resting_orders,waiting_orders\n";
        for (const auto& symbol : symbols) {
            Price initial = initial_prices_.count(symbol) ? initial_prices_.at(symbol) : 0.0;
            Price final_price = final_prices.count(symbol) ? final_prices.at(symbol) : 0.0;
            const SymbolStats& s = stats[symbol];
            auto snap = engine_.snapshot(symbol, std::numeric_limits<std::size_t>::max());
            Quantity bid_depth = 0, ask_depth = 0;
            for (const auto& level : snap.bids) bid_depth += level.quantity;
            for (const auto& level : snap.asks) ask_depth += level.quantity;
            Price best_bid = snap.bids.empty() ? 0.0 : snap.bids.front().price;
            Price best_ask = snap.asks.empty() ? 0.0 : snap.asks.front().price;
            double total_depth = static_cast<double>(bid_depth + ask_depth);

            out << symbol << "," << initial << "," << final_price << "," << (initial > 0.0 ? (final_price - initial) / initial * 100.0 : 0.0) << ",";
            if (s.trades > 0) {
                out << s.high << "," << s.low << "," << s.notional / static_cast<double>(s.volume);
            } 
            else {
                out << ",,";
            }
            out << "," << s.trades << "," << s.volume << "," << s.notional << ",";
            if (!snap.bids.empty()) {
                out << best_bid;
            }
            out << ",";
            if (!snap.asks.empty()) {
                out << best_ask;
            }
            out << ",";
            if (!snap.bids.empty() && !snap.asks.empty()) {
                out << (best_ask - best_bid);
            }
            out << "," << bid_depth << "," << ask_depth << "," << (total_depth > 0.0 ? (static_cast<double>(bid_depth) - static_cast<double>(ask_depth)) / total_depth : 0.0) << "," << resting_by_symbol[symbol].size() << "," << waiting_by_symbol[symbol] << "\n";
        }
    }

    // portfolios_final.csv (before vs after) 
    double total_cash_initial = 0.0, total_cash_final = 0.0, total_reserved_cash_final = 0.0;
    std::size_t negative_cash_clients = 0, negative_share_positions = 0; // both must stay 0, see portfolio.hpp
    {
        auto out = open_csv("portfolios_final");
        out << "client,label,first_seen_ms,initial_cash,initial_net_worth,final_cash,final_reserved_cash,final_available_cash,final_net_worth,pnl,pnl_pct,trades_as_buyer,trades_as_seller,holdings,reserved_shares\n";
        std::vector<ClientId> clients;
        for (const auto& [client, portfolio] : final_portfolios) {
            clients.push_back(client);
        }
        std::sort(clients.begin(), clients.end());
        for (ClientId client : clients){
            const Portfolio& portfolio = final_portfolios.at(client);
            double final_net_worth = portfolio.net_worth(final_prices);
            auto seen_it = first_seen_.find(client);
            FirstSeen seen = (seen_it != first_seen_.end()) ? seen_it->second : FirstSeen{0.0, portfolio.cash, final_net_worth};
            double pnl = final_net_worth - seen.net_worth;
            total_cash_initial += seen.cash;
            total_cash_final += portfolio.cash;
            total_reserved_cash_final += portfolio.reserved_cash;
            if (portfolio.cash < -1e-6) {
                ++negative_cash_clients;
            }
            for (const auto& [symbol, quantity] : portfolio.holdings){
                if (quantity < 0) {
                    ++negative_share_positions;
                }
            }
            auto label_it = labels_.find(client);
            out << client << ',' << (label_it != labels_.end() ? label_it->second : std::string()) << ',' << seen.elapsed_ms << ',' << seen.cash << ',' << seen.net_worth << ',' << portfolio.cash << ',' << portfolio.reserved_cash << ',' << portfolio.available_cash() << ',' << final_net_worth << ',' << pnl << ',' << (seen.net_worth != 0.0 ? pnl / seen.net_worth * 100.0 : 0.0) << ',' << buys_by_client[client] << ',' << sells_by_client[client] << ',' << holdings_to_string(portfolio) << ',' << reserved_to_string(portfolio) << '\n';
        }
    }

    // rejections.csv 
    std::uint64_t total_rejections = 0;
    {
        auto out = open_csv("rejections");
        out << "reason,count\n";
        for (const auto& [reason, count] : rejections_by_reason_) {
            out << reason << "," << count << "\n";
            total_rejections += count;
        }
    }

    // summary.csv 
    {
        auto& metrics = MetricsRegistry::instance();
        double duration_s = elapsed_ms(Clock::now()) / 1000.0;
        auto out = open_csv("summary");
        out << "key,value\n"
            << "duration_s," << duration_s << "\n"
            << "symbols," << symbols.size() << "\n"
            << "clients," << final_portfolios.size() << "\n"
            << "trades," << trades_.size() << "\n"
            << "volume," << total_volume << "\n"
            << "notional," << total_notional << "\n"
            << "orders_submitted," << metrics.orders_submitted() << "\n"
            << "orders_accepted," << metrics.orders_accepted() << "\n"
            << "orders_rejected," << total_rejections << "\n"
            << "orders_queued," << queued_events_ << "\n"
            << "orders_expired," << expired_events_ << "\n"
            << "resting_orders_final," << total_resting << "\n"
            << "waiting_orders_final," << waiting.size() << "\n"
            << "mean_submit_latency_us," << metrics.submit_latency().mean_us() << "\n"
            << "max_submit_latency_us," << metrics.submit_latency().max_us() << "\n"
            << "total_cash_initial," << total_cash_initial << "\n"
            << "total_cash_final," << total_cash_final << "\n"
            << "total_reserved_cash_final," << total_reserved_cash_final << "\n"
            << "negative_cash_clients," << negative_cash_clients << "\n"
            << "negative_share_positions," << negative_share_positions << "\n"
            << "sample_interval_ms," << config_.sample_interval.count() << "\n";
    }

    if (ok) {
        LOG_INFO("market report written to ", display_path(config_.output_dir), " (", trades_.size(), " trades, ", total_resting, " resting, ", waiting.size(), " waiting orders)");
    }
    return ok;
}

} // namespace sim
