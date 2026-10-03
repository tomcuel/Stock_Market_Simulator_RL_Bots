#include "net/simulation_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "logger.hpp"
#include "metrics.hpp"
#include "net/persistence.hpp"
#include "net/protocol.hpp"

namespace sim::net {

namespace {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// parses a "KEY=VALUE" token list (everything after the fixed ORDER prefix) into a lookup map
// Malformed tokens (no '=') are simply ignored rather than rejecting the whole command, so unknown future keys don't break older clients
std::unordered_map<std::string, std::string> parse_kv(const std::vector<std::string>& tokens, std::size_t start) {
    std::unordered_map<std::string, std::string> kv;
    for (std::size_t i = start; i < tokens.size(); ++i) {
        auto eq = tokens[i].find('=');
        if (eq == std::string::npos) {
            continue;
        }
        kv[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
    }
    return kv;
}

std::optional<double> kv_double(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) {
        return std::nullopt;
    }
    try {
        return std::stod(it->second);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<long long> kv_int(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) {
        return std::nullopt;
    }
    try {
        return std::stoll(it->second);
    } catch (...) {
        return std::nullopt;
    }
}

OrderKind parse_kind(const std::string& text, bool& ok) {
    ok = true;
    if (text == "MARKET") {
        return OrderKind::MARKET;
    }
    if (text == "LIMIT") {
        return OrderKind::LIMIT;
    }
    if (text == "STOP") {
        return OrderKind::STOP;
    }
    if (text == "LIMIT_STOP") {
        return OrderKind::LIMIT_STOP;
    }
    ok = false;
    return OrderKind::MARKET;
}

} // namespace

SimulationServer::SimulationServer(ServerConfig config, NotificationBus& bus, MatchingEngine& engine, MarketData& market_data): config_(config), bus_(bus), engine_(engine), market_data_(market_data) {
    log_engine_events();
}

// Subscribes a logger to every engine event so trades and the server's interactions with orders are actually visible in the log 
void SimulationServer::log_engine_events() {
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
    });
}

SimulationServer::~SimulationServer() {
    stop();
}

bool SimulationServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("failed to create listening socket");
        return false;
    }

    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.port));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        LOG_ERROR("failed to bind to port ", config_.port, " (already in use?)");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, /*backlog=*/64) < 0) {
        LOG_ERROR("failed to listen on port ", config_.port);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread([this] {accept_loop();});
    watcher_thread_ = std::thread([this] {watcher_loop();});
    if (!config_.persistence_path.empty()) {
        persistence_thread_ = std::thread([this] {persistence_loop();});
    }
    if (!config_.metrics_csv_path.empty()) {
        metrics_thread_ = std::thread([this] {metrics_loop();});
    }

    LOG_INFO("SimulationServer listening on port ", config_.port);
    return true;
}

void SimulationServer::stop() {
    if (!running_.exchange(false)) {
        return; // already stopped
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }
    if (persistence_thread_.joinable()) {
        persistence_thread_.join();
    }
    if (metrics_thread_.joinable()) {
        metrics_thread_.join();
    }
    if (!config_.persistence_path.empty()) {
        save_snapshot(); // one last save so a graceful shutdown never loses activity since the last periodic save
    }
    // per-connection threads are detached (see handle_connection's spawn site below): each one  exits on its own once its socket errors out or its next recv() times out (kRecvTimeoutSeconds),
    // so stop() doesn't block waiting on however many clients happen to be idling at shutdown time
    LOG_INFO("SimulationServer stopped");
}

void SimulationServer::accept_loop() {
    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd_, &read_fds);
        timeval timeout{1, 0}; // 1 second, so we periodically re-check running_ for shutdown

        int ready = ::select(listen_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue; // timeout or interrupted: loop back and check running_ again
        }

        sockaddr_in client_address{};
        socklen_t client_len = sizeof(client_address);
        int client_socket = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &client_len);
        if (client_socket < 0) {
            continue;
        }

        set_recv_timeout(client_socket, kRecvTimeoutSeconds);
        disable_sigpipe(client_socket); // a client vanishing mid-reply must not kill the server
        std::thread(&SimulationServer::handle_connection, this, client_socket).detach();
    }
}

void SimulationServer::watcher_loop() {
    int tick = 0;
    while (running_.load()) {
        std::this_thread::sleep_for(config_.watcher_poll_interval);
        ++tick;
        // most ticks only rescan symbols that traded since the last tick (cheap, see  MatchingEngine::try_release_waiting_orders):
        // every Nth tick does a full sweep so orders waiting purely on a start date, or expiring on an otherwise-quiet symbol, are still eventually caught
        bool full_scan = (config_.full_scan_every_n_ticks > 0) && (tick % config_.full_scan_every_n_ticks == 0);
        auto summary = engine_.try_release_waiting_orders(full_scan);
        if (!summary.released.empty() || !summary.expired.empty()) {
            LOG_INFO("watcher: released ", summary.released.size(), " order(s), expired ", summary.expired.size(), " order(s)", full_scan ? " (full scan)" : "");
        }
    }
}

void SimulationServer::persistence_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(config_.persistence_save_interval);
        if (!running_.load()) {
            break;
        }
        save_snapshot();
    }
}

void SimulationServer::save_snapshot() const {
    PersistenceStore::save(config_.persistence_path, directory_, engine_);
}

// Appends one CSV row of throughput/latency metrics every metrics_interval, so a server run can be plotted after the fact instead of only ever being visible as a one-shot metrics command response
void SimulationServer::metrics_loop() {
    std::ofstream out(config_.metrics_csv_path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR("metrics recorder: failed to open ", config_.metrics_csv_path, " for writing");
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
    while (running_.load()) {
        std::this_thread::sleep_for(config_.metrics_interval);
        if (!running_.load()) {
            break;
        }
        write_row();
    }
    write_row(); // final row at shutdown, so the series always ends at the end of the session
}

void SimulationServer::handle_connection(int client_socket) {
    ClientId client_id = 0;
    bool authenticated = false;

    while (running_.load()) {
        auto message = recv_framed(client_socket);
        if (!message.has_value()) break; // disconnected, timed out, or protocol violation

        std::vector<std::string> tokens = split(*message);
        if (tokens.empty()) {
            send_framed(client_socket, "ERR EMPTY_COMMAND");
            continue;
        }

        const std::string& command = tokens[0];

        if (!authenticated) {
            if (command == "REGISTER" && tokens.size() == 3) {
                ClientId new_id;
                std::string token;
                auto outcome = directory_.register_client(tokens[1], tokens[2], new_id, token);
                if (outcome == ClientDirectory::RegisterOutcome::OK) {
                    engine_.ensure_client(new_id, 100000.0); // starting cash for a freshly registered account
                    // also grant a small starting position in every currently registered symbol: 
                    // without this, a fresh account could never place a first SELL (nothing to sell) nor have a MARKET BUY fill (nobody else holds shares to sell it either),
                    // since Src_Simulation has no separate "IPO" step the way Src_SQL's demo seeds Client2 with an explicit starting portfolio (see Src_SQL/server.cpp's `init`)
                    for (const auto& symbol : engine_.symbols()) {
                        engine_.grant_initial_holdings(new_id, symbol, 50);
                    }
                    client_id = new_id;
                    authenticated = true;
                    std::ostringstream oss;
                    oss << "OK REGISTERED client_id=" << client_id << " token=" << token;
                    send_framed(client_socket, oss.str());
                    if (config_.log_commands) {
                        LOG_INFO("REGISTER username=", tokens[1], " -> client_id=", client_id);
                    }
                } 
                else {
                    send_framed(client_socket, "ERR USERNAME_TAKEN");
                }
            } 
            else if (command == "LOGIN" && tokens.size() == 3) {
                ClientId existing_id;
                std::string token;
                auto outcome = directory_.authenticate(tokens[1], tokens[2], existing_id, token);
                if (outcome == ClientDirectory::AuthOutcome::OK) {
                    client_id = existing_id;
                    authenticated = true;
                    std::ostringstream oss;
                    oss << "OK LOGIN client_id=" << client_id << " token=" << token;
                    send_framed(client_socket, oss.str());
                    if (config_.log_commands) {
                        LOG_INFO("LOGIN username=", tokens[1], " -> client_id=", client_id);
                    }
                } 
                else if (outcome == ClientDirectory::AuthOutcome::UNKNOWN_USERNAME) {
                    send_framed(client_socket, "ERR UNKNOWN_USERNAME");
                } 
                else {
                    send_framed(client_socket, "ERR WRONG_PASSWORD");
                }
            } 
            else if (command == "RESUME" && tokens.size() == 2) {
                auto resolved = directory_.resolve_token(tokens[1]);
                if (resolved.has_value()) {
                    client_id = *resolved;
                    authenticated = true;
                    std::ostringstream oss;
                    oss << "OK RESUMED client_id=" << client_id;
                    send_framed(client_socket, oss.str());
                    if (config_.log_commands) {
                        LOG_INFO("RESUME -> client_id=", client_id);
                    }
                } 
                else {
                    send_framed(client_socket, "ERR UNKNOWN_TOKEN");
                }
            } 
            else if (command == "QUIT") {
                send_framed(client_socket, "OK BYE");
                break;
            } 
            else {
                send_framed(client_socket, "ERR NOT_AUTHENTICATED (send REGISTER <user> <pass>, LOGIN <user> <pass>, or RESUME <token> first)");
            }
            continue;
        }

        if (command == "QUIT") {
            send_framed(client_socket, "OK BYE");
            break;
        }

        // A single client's bad command must never take down the whole server (and every other client's connection with it)
        // This is a deliberate last-resort safety net on top of fixing the actual bug that motivated it (MatchingEngine::snapshot()/cancel_order() used to throw std::out_of_range, uncaught, for an unregistered symbol)
        // any future command handler that misses a validation check fails this one connection with an error response instead of crashing the process
        std::string response;
        try {
            response = dispatch_command(*message, client_id);
        } catch (const std::exception& e) {
            LOG_ERROR("dispatch_command threw for client ", client_id, ": ", e.what());
            response = std::string("ERR INTERNAL_ERROR ") + e.what();
        } catch (...) {
            LOG_ERROR("dispatch_command threw a non-std::exception for client ", client_id);
            response = "ERR INTERNAL_ERROR";
        }
        // Every client interaction the server deals with, logged: what was asked, who asked, what came back
        // ORDER commands that produced a trade are already covered in more detail by log_engine_events()'s TRADE line(s)
        // this is the complementary "what did the server see and answer" view (including commands that never touch the engine at all, like PORTFOLIO/MARKET/SYMBOLS/METRICS, which log_engine_events() has no visibility into)
        if (config_.log_commands) {
            LOG_INFO("client=", client_id, " command=[", *message, "] response=[", response, "]");
        }
        if (!send_framed(client_socket, response)) break;
    }

    ::close(client_socket);
}

std::string SimulationServer::dispatch_command(const std::string& line, ClientId client_id) {
    std::vector<std::string> tokens = split(line);
    const std::string& command = tokens[0];

    if (command == "ORDER") {
        return handle_order_command(tokens, client_id);
    }

    if (command == "CANCEL" && tokens.size() == 3) {
        try {
            OrderId order_id = std::stoull(tokens[1]);
            const std::string& symbol = tokens[2];
            // tries the resting book first (the common case for a market-maker cancel-and-requote), then falls back to the waiting registry (a STOP/LIMIT_STOP that hasn't released yet)
            if (engine_.cancel_order(symbol, order_id)) {
                return "OK CANCELLED";
            }
            if (engine_.cancel_waiting_order(order_id)) {
                return "OK CANCELLED";
            }
            return "ERR NOT_FOUND";
        } catch (...) {
            return "ERR INVALID_ARGUMENTS";
        }
    }

    if (command == "CANCEL_WAITING" && tokens.size() == 2) {
        try {
            OrderId order_id = std::stoull(tokens[1]);
            return engine_.cancel_waiting_order(order_id) ? "OK CANCELLED" : "ERR NOT_FOUND";
        } catch (...) {
            return "ERR INVALID_ORDER_ID";
        }
    }

    if (command == "CANCEL_BOOK" && tokens.size() == 5) {
        try {
            OrderId order_id = std::stoull(tokens[1]);
            const std::string& symbol = tokens[2];
            Side side = (tokens[3] == "BUY") ? Side::BUY : Side::SELL;
            Price price = std::stod(tokens[4]);
            return engine_.cancel_order(symbol, side, price, order_id) ? "OK CANCELLED" : "ERR NOT_FOUND";
        } catch (...) {
            return "ERR INVALID_ARGUMENTS";
        }
    }

    if (command == "PORTFOLIO") {
        Portfolio portfolio = engine_.portfolio_snapshot(client_id);
        std::ostringstream oss;
        oss << std::setprecision(15) << "OK PORTFOLIO cash=" << portfolio.cash;
        for (const auto& [symbol, qty] : portfolio.holdings) {
            oss << " " << symbol << "=" << qty;
        }
        return oss.str();
    }

    if (command == "MARKET" && tokens.size() == 2) {
        const std::string& symbol = tokens[1];
        auto snap = engine_.snapshot(symbol, 1);
        // MarketData only knows about prices from actual trades: before the first trade on a symbol, fall back to the engine's registered reference price, so `last` is never misleadingly 0
        // (the same one pre-trade risk checks and STOP/LIMIT_STOP release bands use)
        Price last = market_data_.last_price(symbol);
        if (last <= 0.0) last = engine_.last_price(symbol);
        std::ostringstream oss;
        oss << std::setprecision(15)
            << "OK MARKET " << symbol << " last=" << last
            << " vwap=" << market_data_.vwap(symbol) << " volume=" << market_data_.total_volume(symbol);
        if (!snap.bids.empty()) oss << " bid=" << snap.bids.front().price;
        if (!snap.asks.empty()) oss << " ask=" << snap.asks.front().price;
        return oss.str();
    }

    if (command == "SYMBOLS") {
        std::ostringstream oss;
        oss << "OK SYMBOLS";
        for (const auto& symbol : engine_.symbols()) {
            oss << " " << symbol;
        }
        return oss.str();
    }

    if (command == "METRICS") {
        std::ostringstream oss;
        oss << "OK METRICS " << MetricsRegistry::instance().report(0.0) << " waiting_orders=" << engine_.waiting_order_count();
        return oss.str();
    }

    return "ERR UNKNOWN_COMMAND";
}

std::string SimulationServer::handle_order_command(const std::vector<std::string>& tokens, ClientId client_id) {
    if (tokens.size() < 5) {
        return "ERR ORDER reason=usage:_ORDER_<BUY|SELL>_<SYMBOL>_<QTY>_<KIND>_[KEY=VALUE...]";
    }

    OrderRequest request;
    request.client = client_id;
    request.side = (tokens[1] == "BUY") ? Side::BUY : Side::SELL;
    if (tokens[1] != "BUY" && tokens[1] != "SELL") {
        return "ERR ORDER reason=side_must_be_BUY_or_SELL";
    }
    request.symbol = tokens[2];

    try {
        request.quantity = std::stoll(tokens[3]);
    } catch (...) {
        return "ERR ORDER reason=invalid_quantity";
    }

    bool kind_ok = false;
    request.kind = parse_kind(tokens[4], kind_ok);
    if (!kind_ok) {
        return "ERR ORDER reason=kind_must_be_MARKET_LIMIT_STOP_or_LIMIT_STOP";
    }

    auto kv = parse_kv(tokens, 5);
    if (auto price = kv_double(kv, "PRICE")) {
        request.price = *price;
    }
    if (auto trigger = kv_double(kv, "TRIGGER")) {
        request.trigger_upper = *trigger; // STOP shorthand
    }
    if (auto lower = kv_double(kv, "TRIGGER_LOWER")) {
        request.trigger_lower = *lower;
    }
    if (auto upper = kv_double(kv, "TRIGGER_UPPER")) {
        request.trigger_upper = *upper;
    }
    if (auto expires = kv_int(kv, "EXPIRES")) {
        request.expires_in = std::chrono::seconds(*expires);
    }
    if (auto not_before = kv_int(kv, "NOT_BEFORE")) {
        request.not_before_in = std::chrono::seconds(*not_before);
    }

    // STOP's resting price defaults to its trigger if the client didn't say otherwise -- most
    // callers only care about the trigger level, not a separate resting price
    if (request.kind == OrderKind::STOP && request.price <= 0.0 && request.trigger_upper.has_value()) {
        request.price = *request.trigger_upper;
    }

    SubmitResult result = engine_.submit_order(request);

    std::ostringstream oss;
    if (!result.accepted) {
        oss << "ERR ORDER order_id=" << result.order_id << " reason=" << result.reject_reason;
        return oss.str();
    }
    if (result.queued) {
        oss << "OK ORDER order_id=" << result.order_id << " status=QUEUED";
        return oss.str();
    }
    if (result.filled_quantity > 0) {
        oss << "OK ORDER order_id=" << result.order_id << " status=FILLED filled=" << result.filled_quantity << " trades=" << result.trades.size();
    } 
    else if (request.kind == OrderKind::MARKET) {
        // an IOC order with nothing to match against: accepted, but there is nothing left of it
        oss << "OK ORDER order_id=" << result.order_id << " status=NO_LIQUIDITY filled=0";
    } 
    else {
        // LIMIT/STOP/LIMIT_STOP accepted and released into the book, but didn't cross anything yet
        oss << "OK ORDER order_id=" << result.order_id << " status=RESTING filled=0";
    }
    return oss.str();
}

} // namespace sim::net
