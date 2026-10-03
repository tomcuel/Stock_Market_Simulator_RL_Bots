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
//   --output-dir DIR         write run files into DIR (created if missing). Every CSV ends in "_bots" so it never collides with simulation.x's "_simu" files
//   --no-metrics             with --output-dir: skip metrics_bots.csv
//   --no-report              with --output-dir: skip the market report (written on shutdown)
//   --metrics-interval-ms N  metrics sampling period (default 1000)
//   --report-interval-ms N   market report sampling period (default 250)
//   --snapshot PATH          persistence: restore accounts/portfolios/prices from PATH at startup and save them there periodically and on shutdown (disabled by default)
//   --save-interval SEC      snapshot save period (default 30)
//=======================================================================
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "logger.hpp"
#include "market_data.hpp"
#include "market_recorder.hpp"
#include "market_seed.hpp"
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
    const std::string file_suffix = "_bots"; // every CSV this server writes ends in _bots.csv

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };

        if (arg == "--port") port = std::atoi(next().c_str());
        else if (arg == "--snapshot") {
            snapshot_path = next();
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
                      << "  --data-seed PATH         real-market seed CSV, from Data/feature_engineering.py (default: " << kDefaultDataSeedPath << ")\n"
                      << "  --no-data-seed           skip real-data seeding, use 4 hardcoded demo symbols instead\n"
                      << "  --max-symbols N          how many tickers to load from the seed CSV (default 8)\n"
                      << "  --log-level LEVEL        one of DEBUG, INFO, WARN, ERROR (default INFO). INFO and above shows every trade and every client command/response\n"
                      << "  --no-log-commands        stop logging every client command + response (trades are still logged; this only trims PORTFOLIO/MARKET/etc. noise)\n"
                      << "  --output-dir DIR         write metrics_bots.csv and, on shutdown, the market report (*_bots.csv) into DIR (created if missing)\n"
                      << "  --no-metrics             with --output-dir: skip metrics_bots.csv\n"
                      << "  --no-report              with --output-dir: skip the market report\n"
                      << "  --metrics-interval-ms N  metrics sampling period (default 1000)\n"
                      << "  --report-interval-ms N   report sampling period (default 250)\n\n"
                      << "Plot the output with: python3 scripts/plot_metrics.py DIR && python3 scripts/plot_market_report.py DIR\n"
                      << "or run server + bots + plots at once with ./launch.sh bots\n";
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
        std::cout << "No usable data seed found at " << data_seed_path << " -- using 4 hardcoded demo symbols\n" << "(Run Data/preprocess.py && Data/feature_engineering.py first for real market data)\n";
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
            std::cerr << "Cannot create output directory " << output_dir << ": " << ec.message() << "\n";
            return EXIT_FAILURE;
        }
        if (write_metrics) {
            config.metrics_csv_path = (std::filesystem::path(output_dir) / ("metrics" + file_suffix + ".csv")).string();
        }
    }
    config.metrics_interval = std::chrono::milliseconds(metrics_interval_ms);
    config.log_commands = log_commands;
    sim::net::SimulationServer server(config, bus, engine, market_data);

    if (!snapshot_path.empty()) {
        sim::net::PersistenceStore::load(snapshot_path, server.client_directory(), engine);
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
        std::cout << "Persistence: saving to " << snapshot_path << " every " << save_interval_sec << "s\n";
    }
    if (!config.metrics_csv_path.empty()) {
        std::cout << "Metrics: appending to " << config.metrics_csv_path << " every " << metrics_interval_ms << "ms\n";
    }
    if (recorder) {
        std::cout << "Market report: recording, will be written to " << output_dir << " (*" << file_suffix << ".csv) on shutdown\n";
    }
    std::cout << "Press Ctrl-C to stop\n";

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "Shutting down...\n";
    server.stop();
    if (recorder) {
        recorder->stop();
        if (recorder->write_report()) {
            std::cout << "Market report written to " << output_dir << " (plot with: python3 scripts/plot_market_report.py " << output_dir << ")\n";
        } 
        else {
            std::cerr << "Failed to write the market report to " << output_dir << " (see log)\n";
        }
    }
    return EXIT_SUCCESS;
}
