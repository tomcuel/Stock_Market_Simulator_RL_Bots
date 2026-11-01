//=======================================================================
// sim_server.x: the exchange server of the client-server architecture
// It owns the order books and portfolios: clients (humans via sim_client.x, or sim_client.x --bot) trade over TCP
// (Runs until Ctrl-C / SIGTERM)
//
// Quick start (from Src_Simulation/, after `make`):
//   ./sim_server.x                                  # port 7878, symbols from the data seed, log to stdout
//   ./sim_server.x --output-dir output/my_session   # also write, on shutdown:
//                                                   #   output/my_session/metrics_bots.csv
//                                                   #   output/my_session/{summary,symbols,trades,...}_bots.csv
// The easiest way to run it with bots, logs and plots is the launcher: ./launch.sh bots
//
// Options (all optional; defaults give a working server):
//   --port N                 listen port (default 7878)
//   --max-symbols N          tickers taken from the data seed (default 8)
//   --data-seed PATH         real-market seed CSV (default ../Data/Datasets/processed/latest_snapshot.csv)
//   --no-data-seed           use 4 built-in demo tickers (AAPL MSFT GOOG TSLA) instead
//   --log-level LEVEL        DEBUG | INFO | WARN | ERROR (default INFO: every trade + every client command)
//   --no-log-commands        keep trade/rejection logging but drop the per-command lines
//   --starting-equity X      value of every newly registered account (default 10000000)
//   --cash-fraction F        share of it in cash, the rest split equally by value across the symbols (default 0.5)
//   --output-dir DIR         write run files into DIR (created if missing). Every CSV ends in "_bots" so it never collides with simulation.x's "_simu" files
//   --no-metrics             with --output-dir: skip metrics_bots.csv
//   --no-report              with --output-dir: skip the market report (written on shutdown)
//   --metrics-interval-ms N  metrics sampling period (default 1000)
//   --report-interval-ms N   market report sampling period (default 250)
//   --snapshot PATH          persistence: restore accounts/portfolios/prices from PATH at startup and save them there periodically and on shutdown (disabled by default)
//   --save-interval SEC      snapshot save period (default 30)
//   --closing-auction SEC    end the trading day when stopped: SEC seconds of pre-close (orders accepted, nothing matches), then the closing auction sets each symbol's official close, every order left expires (day orders) and the snapshot is saved with those closing prices (0: no pre-close, just the expiry, default: off, the server stops at once)
//=======================================================================
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "logger.hpp"
#include "market_data.hpp"
#include "market_recorder.hpp"
#include "market_seed.hpp"
#include "paths.hpp"
#include "matching_engine.hpp"
#include "net/persistence.hpp"
#include "net/simulation_server.hpp"
#include "notification.hpp"

namespace {
std::atomic<bool> g_shutdown_requested{false};
void handle_sigint(int) { g_shutdown_requested.store(true); }

const char* kDefaultDataSeedPath = "../Data/Datasets/processed/latest_snapshot.csv";

sim::LogLevel parse_log_level(const std::string& level) {
    if (level == "DEBUG") return sim::LogLevel::DEBUG;
    if (level == "WARN") return sim::LogLevel::WARN;
    if (level == "ERROR") return sim::LogLevel::ERROR;
    return sim::LogLevel::INFO;
}
} // namespace

int main(int argc, char* argv[]) {
    int port = 7878;
    std::string snapshot_path;
    int closing_auction_sec = -1; // off
    int save_interval_sec = 30;
    std::string data_seed_path = kDefaultDataSeedPath;
    bool use_data_seed = true;
    std::size_t max_data_symbols = 8;
    std::string log_level = "INFO";
    int metrics_interval_ms = 1000;
    bool log_commands = true;
    std::string output_dir;
    bool write_metrics = true;
    bool write_report = true;
    int report_interval_ms = 250;
    double starting_equity = 10000000.0;
    double cash_fraction = 0.5;
    const std::string file_suffix = "_bots"; // every CSV this server writes ends in _bots.csv

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {return (i + 1 < argc) ? argv[++i] : "";};

        if (arg == "--port") port = std::atoi(next().c_str());
        else if (arg == "--snapshot") {
            snapshot_path = next();
        }
        else if (arg == "--closing-auction") {
            closing_auction_sec = std::max(0, std::atoi(next().c_str()));
        }
        else if (arg == "--save-interval") {
            save_interval_sec = std::atoi(next().c_str());
        }
        else if (arg == "--data-seed") {
            data_seed_path = next();
        }
        else if (arg == "--no-data-seed") {
            use_data_seed = false;
        }
        else if (arg == "--max-symbols") {
            max_data_symbols = static_cast<std::size_t>(std::atoi(next().c_str()));
        }
        else if (arg == "--log-level") {
            log_level = next();
        }
        else if (arg == "--no-log-commands") {
            log_commands = false;
        }
        else if (arg == "--starting-equity") {
            starting_equity = std::max(1.0, std::atof(next().c_str()));
        }
        else if (arg == "--cash-fraction") {
            cash_fraction = std::clamp(std::atof(next().c_str()), 0.0, 1.0);
        }
        else if (arg == "--output-dir") {
            output_dir = next();
        }
        else if (arg == "--no-metrics") {
            write_metrics = false;
        }
        else if (arg == "--no-report") {
            write_report = false;
        }
        else if (arg == "--metrics-interval-ms") {
            metrics_interval_ms = std::max(10, std::atoi(next().c_str()));
        }
        else if (arg == "--report-interval-ms") {
            report_interval_ms = std::max(10, std::atoi(next().c_str()));
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --port N                listen port (default 7878)\n"
                      << "  --snapshot PATH          enable persistence: save/restore accounts, portfolios, and last prices to PATH across restarts (disabled by default)\n"
                      << "  --save-interval SEC      how often to save the snapshot (default 30)\n"
                      << "  --closing-auction SEC    on stop: SEC s of pre-close, the closing auction, day orders expire, then the snapshot keeps the closing prices (default: off)\n"
                      << "  --data-seed PATH         real-market seed CSV, from Data/feature_engineering.py (default: " << kDefaultDataSeedPath << ")\n"
                      << "  --no-data-seed           skip real-data seeding, use 4 hardcoded demo symbols instead\n"
                      << "  --max-symbols N          how many tickers to load from the seed CSV (default 8)\n"
                      << "  --log-level LEVEL        one of DEBUG, INFO, WARN, ERROR (default INFO). INFO and above shows every trade and every client command/response\n"
                      << "  --no-log-commands        stop logging every client command + response (trades are still logged, this only trims PORTFOLIO/MARKET/etc. noise)\n"
                      << "  --starting-equity X      value of every newly registered account (default 10000000)\n"
                      << "  --cash-fraction F        share of it in cash, the rest split equally by value (default 0.5)\n"
                      << "  --output-dir DIR         write metrics_bots.csv and, on shutdown, the market report (*_bots.csv) into DIR (created if missing)\n"
                      << "  --no-metrics             with --output-dir: skip metrics_bots.csv\n"
                      << "  --no-report              with --output-dir: skip the market report\n"
                      << "  --metrics-interval-ms N  metrics sampling period (default 1000)\n"
                      << "  --report-interval-ms N   report sampling period (default 250)\n\n"
                      << "Plot the output with: python3 scripts/plot_metrics.py DIR && python3 scripts/plot_market_report.py DIR or run server + bots + plots at once with ./launch.sh bots\n";
            return EXIT_SUCCESS;
        }
    }

    sim::Logger::instance().set_min_level(parse_log_level(log_level));

    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);
    // a client disconnecting mid-reply must never terminate the server
    std::signal(SIGPIPE, SIG_IGN);

    sim::NotificationBus bus;
    sim::MatchingEngine engine(bus);
    sim::MarketData market_data(bus);

    // Seed symbols from real Yahoo-Finance-derived data (Data/enter_in_database.py's sibling output, Data/feature_engineering.py's latest_snapshot.csv) when available, 
    // falling back to a small hardcoded demo list otherwise: so a from-scratch checkout still runs without requiring the Data/ pipeline to have been run first
    std::vector<std::pair<sim::Symbol, sim::Price>> symbols;
    std::vector<sim::SeedSymbol> data_symbols;
    if (use_data_seed) {
        data_symbols = sim::pick_symbols(sim::load_market_seed(data_seed_path), max_data_symbols);
    }
    if (!data_symbols.empty()) {
        for (const auto& seed : data_symbols) {
            symbols.emplace_back(seed.symbol, seed.last_price);
        }
        std::cout << "Seeded " << symbols.size() << " symbols from " << data_seed_path << "\n";
    } 
    else {
        symbols = {{"AAPL", 180.0}, {"MSFT", 410.0}, {"GOOG", 165.0}, {"TSLA", 240.0}};
        std::cout << "No usable data seed found at " << sim::display_path(data_seed_path) << ": using 4 hardcoded demo symbols\n" << "(Run Data/preprocess.py && Data/feature_engineering.py first for real market data)\n";
    }

    for (const auto& [symbol, price] : symbols) {
        engine.register_symbol(symbol, price);
    }

    sim::net::ServerConfig config;
    config.port = port;
    config.persistence_path = snapshot_path;
    config.persistence_save_interval = std::chrono::seconds(save_interval_sec);
    if (!output_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(output_dir, ec);
        if (ec) {
            std::cerr << "Cannot create output directory " << sim::display_path(output_dir) << ": " << ec.message() << "\n";
            return EXIT_FAILURE;
        }
        if (write_metrics) {
            config.metrics_csv_path = (std::filesystem::path(output_dir) / ("metrics" + file_suffix + ".csv")).string();
        }
    }
    config.metrics_interval = std::chrono::milliseconds(metrics_interval_ms);
    config.log_commands = log_commands;
    config.starting_equity = starting_equity;
    config.starting_cash_fraction = cash_fraction;
    sim::net::SimulationServer server(config, bus, engine, market_data);

    if (!snapshot_path.empty()) {
        sim::net::PersistenceStore::load(snapshot_path, server.client_directory(), engine);
        // orders carried over from the previous session may cross each other: the session opens with an auction
        if (!engine.open_orders().empty()) {
            std::cout << "Opening auction on " << engine.open_orders().size() << " carried-over order(s):\n";
            for (const auto& open : engine.run_opening_auction()) {
                if (open.crossed) {
                    std::cout << "  open " << open.symbol << " " << open.price << " (" << open.volume << " shares)\n";
                }
            }
        }
    }

    // declared after bus/engine (it observes both) so it's destroyed before them:  it unsubscribes from the bus in its destructor
    std::unique_ptr<sim::MarketRecorder> recorder;
    if (!output_dir.empty() && write_report) {
        sim::RecorderConfig recorder_config;
        recorder_config.output_dir = output_dir;
        recorder_config.file_suffix = file_suffix;
        recorder_config.sample_interval = std::chrono::milliseconds(report_interval_ms);
        recorder = std::make_unique<sim::MarketRecorder>(bus, engine, recorder_config);
        recorder->start(); // "before" snapshot: initial prices + any clients restored from --snapshot
    }

    if (!server.start()) {
        std::cerr << "Failed to start server on port " << port << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Server running on port " << port << ". Symbols: ";
    for (const auto& [symbol, price] : symbols) {
        std::cout << symbol << " ";
    }
    std::cout << "\n";
    if (!snapshot_path.empty()) {
        std::cout << "Persistence: saving to " << sim::display_path(snapshot_path) << " every " << save_interval_sec << "s\n";
    }
    if (!config.metrics_csv_path.empty()) {
        std::cout << "Metrics: appending to " << sim::display_path(config.metrics_csv_path) << " every " << metrics_interval_ms << "ms\n";
    }
    if (recorder) {
        std::cout << "Market report: recording, will be written to " << sim::display_path(output_dir) << " (*" << file_suffix << ".csv) on shutdown\n";
    }
    std::cout << "Press Ctrl-C to stop\n";

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "Shutting down...\n";
    if (closing_auction_sec >= 0) {
        // the end of the trading day, while clients are still connected (the snapshot is saved by server.stop() below)
        if (closing_auction_sec > 0) {
            std::cout << "Pre-close: " << closing_auction_sec << "s of order collection (nothing matches), then the closing auction\n";
            engine.set_auction_mode(true);
            std::this_thread::sleep_for(std::chrono::seconds(closing_auction_sec));
        }
        engine.close_market(); // no new order from here: the auction only executes what the books hold
        auto closes = engine.run_closing_auction();
        bus.flush(); // the auction's trades are logged by the bus thread: let it finish, so they don't interleave with the closes
        for (const auto& close : closes) {
            std::cout << "  close " << close.symbol << " " << close.price << (close.crossed ? " (closing auction, " + std::to_string(close.volume) + " shares)" : " (no cross: last traded price)") << "\n";
        }
        std::size_t expired = engine.expire_day_orders();
        bus.flush();
        std::cout << "  " << expired << " DAY order(s) expired (reservations released), " << engine.open_orders().size() << " GTC / dated order(s) carried over to the next session\n";    
    }
    server.stop();
    if (recorder) {
        recorder->stop();
        for (const auto& account : server.client_directory().export_accounts()) {
            recorder->set_client_label(account.client_id, account.username); // launch_bots.sh names bots botNN_<strategy>
        }
        if (recorder->write_report()) {
            std::cout << "Market report written to " << sim::display_path(output_dir) << " (plot, from the project root: python3 Src_Simulation/scripts/plot_market_report.py " << sim::display_path(output_dir) << ")\n";
        } else {
            std::cerr << "Failed to write the market report to " << sim::display_path(output_dir) << " (see log)\n";
        }
    }
    return EXIT_SUCCESS;
}
