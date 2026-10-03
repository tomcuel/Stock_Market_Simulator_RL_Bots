//=======================================================================
// The server half of Src_Simulation's client-server architecture: a multi-threaded TCP server exposing MatchingEngine/MarketData over the length-prefixed text protocol defined here
// so any process (a human at sim_client.x or a bot running the exact same client code) interacts with the market purely through commands, never by linking against the engine directly 
// This mirrors Src_SQL's server.cpp/client_account.cpp split, and applies the concurrency lessons from Test_Functionnalities/Sockets and Test_Functionnalities/Mutex/Separated_Mutex 
// (every subsystem (books, portfolios, prices, the waiting-order registry, the client directory) already has, or gets, its own lock rather than one global one)
//
// Wire protocol:
//   REGISTER <username> <password>
//   LOGIN <username> <password>
//   RESUME <token>
//   ORDER <BUY|SELL> <SYMBOL> <QTY> <MARKET|LIMIT|STOP|LIMIT_STOP> [PRICE=p] [TRIGGER=t]
//         [TRIGGER_LOWER=l] [TRIGGER_UPPER=u] [EXPIRES=seconds] [NOT_BEFORE=seconds]
//   CANCEL <order_id> <SYMBOL>
//   CANCEL_WAITING <order_id>
//   CANCEL_BOOK <order_id> <SYMBOL> <BUY|SELL> <price>
//   PORTFOLIO
//   MARKET <SYMBOL>
//   SYMBOLS
//   METRICS
//   QUIT
// Every response starts with OK or ERR. REGISTER/LOGIN/RESUME must succeed before any other command is accepted on that connection (ERR NOT_AUTHENTICATED otherwise)
// REGISTER and LOGIN responses include a session token (`OK REGISTERED client_id=1 token=...`)
// reconnecting with RESUME <token> re-authenticates without sending the password again
//=======================================================================
#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "market_data.hpp"
#include "matching_engine.hpp"
#include "net/auth.hpp"
#include "notification.hpp"
#include "types.hpp"

namespace sim::net {

struct ServerConfig {
    int port{7878};
    std::chrono::milliseconds watcher_poll_interval{100};    // how often a dirty-symbol-only release scan runs
    int full_scan_every_n_ticks{10};                          // periodic full release/expiry sweep
    std::string persistence_path;                             // empty = persistence disabled
    std::chrono::seconds persistence_save_interval{30};
    std::string metrics_csv_path;                             // empty = disabled; periodic CSV export, see scripts/plot_metrics.py
    std::chrono::milliseconds metrics_interval{1000};
    bool log_commands{true}; // log every command a client sends and the response it got (INFO level)
};

class SimulationServer {
public:
    SimulationServer(ServerConfig config, NotificationBus& bus, MatchingEngine& engine, MarketData& market_data);
    ~SimulationServer();

    SimulationServer(const SimulationServer&) = delete;
    SimulationServer& operator=(const SimulationServer&) = delete;

    // Binds and starts accepting connections: returns false on a socket error (port already in use, permission denied, ...)
    // Spawns the accept loop, the release-watcher thread, and (if persistence_path is set) the periodic snapshot thread as background threads and returns immediately
    // call stop() (or destroy the server) to shut down
    bool start();
    // Stops all background threads; if persistence is enabled, saves one final snapshot first
    void stop();

    ClientDirectory& client_directory() { return directory_; }

private:
    void accept_loop();
    void handle_connection(int client_socket);
    void watcher_loop();
    void persistence_loop();
    void metrics_loop();
    void save_snapshot() const;
    void log_engine_events(); // subscribes a logger to the notification bus: trades, rejections, queued/expired orders

    // returns the response line for a single already-authenticated command
    std::string dispatch_command(const std::string& line, ClientId client_id);
    std::string handle_order_command(const std::vector<std::string>& tokens, ClientId client_id);

    ServerConfig config_;
    NotificationBus& bus_;
    MatchingEngine& engine_;
    MarketData& market_data_;
    ClientDirectory directory_;

    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::thread watcher_thread_;
    std::thread persistence_thread_;
    std::thread metrics_thread_;
};

} // namespace sim::net
