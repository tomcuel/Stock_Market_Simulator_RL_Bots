//=======================================================================
// simulation.x: in-process market simulation (no sockets): 
// a MatchingEngine plus a pool of bots (noise traders, momentum traders, market makers) that each trade across all symbols, for a fixed duration
// Prints a text report at the end and with --output-dir, writes CSV files for plotting
//
// Quick start (from Src_Simulation/, after `make`):
//   ./simulation.x                                    # 3 symbols, 10s, prints a report, writes no files
//   ./simulation.x --output-dir output/my_run         # same, plus metrics + market report CSVs:
//                                                     #   output/my_run/metrics_simu.csv
//                                                     #   output/my_run/{summary,symbols,trades,...}_simu.csv
//   ./simulation.x --symbols 8 --duration 300 --noise 60 --momentum 20 --marketmakers 12 --output-dir output/big_run # a much bigger run
// The easiest way to run it with logs + plots is the launcher: ./launch.sh simu (see its header)
//
// Options (all optional; defaults give a working run):
//   --symbols N              symbols to simulate (default 3), taken from --data-seed when available, otherwise from a built-in list of 8 demo tickers
//   --duration SECONDS       run length (default 10)
//   --noise N                noise-trader bots (default 15)
//   --momentum N             momentum bots (default 5)
//   --marketmakers N         market-maker bots (default 4)
//   --seed N                 RNG seed, for reproducible runs (default 42)
//   --log-level LEVEL        DEBUG | INFO | WARN | ERROR (default INFO; INFO logs every trade)
//   --data-seed PATH         real-market seed CSV from Data/feature_engineering.py (default ../Data/Datasets/processed/latest_snapshot.csv)
//   --no-data-seed           ignore the seed CSV and use the built-in demo tickers
//   --output-dir DIR         write run files into DIR (created if missing): every CSV ends in "_simu" so it never collides with sim_server.x's "_bots" files
//   --no-metrics             with --output-dir: skip metrics_simu.csv
//   --no-report              with --output-dir: skip the market report CSVs
//   --metrics-interval-ms N  metrics sampling period (default 1000)
//   --report-interval-ms N   market report sampling period for prices/book/portfolios (default 250)
//=======================================================================
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "logger.hpp"
#include "market_seed.hpp"
#include "simulation.hpp"

namespace {

const char* kDefaultDataSeedPath = "../Data/Datasets/processed/latest_snapshot.csv";

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "In-process market simulation. With no options: 3 symbols, 10s, text report only.\n\n"
              << "  --symbols N              symbols to simulate (default 3)\n"
              << "  --duration SECONDS       run length (default 10)\n"
              << "  --noise N                noise-trader bots, trading across ALL symbols (default 15)\n"
              << "  --momentum N             momentum bots (default 5)\n"
              << "  --marketmakers N         market-maker bots (default 4)\n"
              << "  --seed N                 RNG seed for reproducible runs (default 42)\n"
              << "  --log-level LEVEL        DEBUG, INFO, WARN, ERROR (default INFO)\n"
              << "  --data-seed PATH         real-market seed CSV (default: " << kDefaultDataSeedPath << ")\n"
              << "  --no-data-seed           use the built-in demo tickers instead\n"
              << "  --output-dir DIR         write metrics_simu.csv and the market report (*_simu.csv) into DIR\n"
              << "  --no-metrics             with --output-dir: skip metrics_simu.csv\n"
              << "  --no-report              with --output-dir: skip the market report\n"
              << "  --metrics-interval-ms N  metrics sampling period (default 1000)\n"
              << "  --report-interval-ms N   report sampling period (default 250)\n\n"
              << "Plot the output with: python3 scripts/plot_metrics.py DIR && python3 scripts/plot_market_report.py DIR\n"
              << "or run everything at once with ./launch.sh simu\n";
}

sim::LogLevel parse_log_level(const std::string& level) {
    if (level == "DEBUG") {
        return sim::LogLevel::DEBUG;
    }
    if (level == "WARN") {
        return sim::LogLevel::WARN;
    }
    if (level == "ERROR") {
        return sim::LogLevel::ERROR;
    }
    return sim::LogLevel::INFO;
}

} // namespace

int main(int argc, char* argv[]) {
    int num_symbols = 3;
    int duration_seconds = 10;
    unsigned seed = 42;
    std::string log_level = "INFO";
    std::string data_seed_path = kDefaultDataSeedPath;
    bool use_data_seed = true;
    int noise_traders = 15;
    int momentum_traders = 5;
    int market_makers = 4;
    std::string output_dir;
    bool write_metrics = true;
    bool write_report = true;
    int metrics_interval_ms = 1000;
    int report_interval_ms = 250;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << "\n";
                std::exit(EXIT_FAILURE);
            }
            return argv[++i];
        };

        if (arg == "--symbols") {
            num_symbols = std::max(1, std::atoi(next_value("--symbols").c_str()));
        } 
        else if (arg == "--duration") {
            duration_seconds = std::max(1, std::atoi(next_value("--duration").c_str()));
        } 
        else if (arg == "--seed") {
            seed = static_cast<unsigned>(std::atoi(next_value("--seed").c_str()));
        } 
        else if (arg == "--log-level") {
            log_level = next_value("--log-level");
        } 
        else if (arg == "--data-seed") {
            data_seed_path = next_value("--data-seed");
        } 
        else if (arg == "--no-data-seed") {
            use_data_seed = false;
        } 
        else if (arg == "--noise") {
            noise_traders = std::max(0, std::atoi(next_value("--noise").c_str()));
        } 
        else if (arg == "--momentum") {
            momentum_traders = std::max(0, std::atoi(next_value("--momentum").c_str()));
        } 
        else if (arg == "--marketmakers") {
            market_makers = std::max(0, std::atoi(next_value("--marketmakers").c_str()));
        } 
        else if (arg == "--output-dir") {
            output_dir = next_value("--output-dir");
        } 
        else if (arg == "--no-metrics") {
            write_metrics = false;
        } 
        else if (arg == "--no-report") {
            write_report = false;
        } 
        else if (arg == "--metrics-interval-ms") {
            metrics_interval_ms = std::max(10, std::atoi(next_value("--metrics-interval-ms").c_str()));
        } 
        else if (arg == "--report-interval-ms") {
            report_interval_ms = std::max(10, std::atoi(next_value("--report-interval-ms").c_str()));
        } 
        else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } 
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    sim::Logger::instance().set_min_level(parse_log_level(log_level));

    sim::SimulationConfig config;
    config.seed = seed;
    config.duration = std::chrono::seconds(duration_seconds);
    config.noise_traders = noise_traders;
    config.momentum_traders = momentum_traders;
    config.market_makers = market_makers;
    config.output_dir = output_dir;
    config.file_suffix = "_simu";
    config.write_metrics = write_metrics;
    config.write_report = write_report;
    config.metrics_interval = std::chrono::milliseconds(metrics_interval_ms);
    config.report_interval = std::chrono::milliseconds(report_interval_ms);

    // Seed from real Yahoo-Finance-derived prices/volatility (Data/feature_engineering.py's latest_snapshot.csv) when available
    // fall back to a small hardcoded demo list otherwise, so a from-scratch checkout still runs without requiring the Data/ pipeline to have been run
    std::vector<sim::SeedSymbol> data_symbols;
    if (use_data_seed) {
        data_symbols = sim::pick_symbols(sim::load_market_seed(data_seed_path), static_cast<std::size_t>(num_symbols));
    }

    if (!data_symbols.empty()) {
        std::cout << "Seeding " << data_symbols.size() << " symbols from " << data_seed_path << "\n";
        for (const auto& seed_symbol : data_symbols) {
            sim::SymbolConfig symbol_config;
            symbol_config.symbol = seed_symbol.symbol;
            symbol_config.initial_price = seed_symbol.last_price;
            symbol_config.volatility = (seed_symbol.rolling_volatility_20 > 0.0) ? seed_symbol.rolling_volatility_20 : 0.20;
            config.symbols.push_back(symbol_config);
        }
    } 
    else {
        std::cout << "No usable data seed found at " << data_seed_path << " -- using hardcoded demo symbols.\n" << "(Run Data/preprocess.py && Data/feature_engineering.py first for real market data.)\n";
        static const char* kNames[] = {"AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA", "META", "NFLX"};
        static const double kPrices[] = {180.0, 410.0, 165.0, 175.0, 240.0, 900.0, 480.0, 600.0};
        int usable_symbols = std::min(num_symbols, 8); // the hardcoded list only has 8 distinct names

        for (int i = 0; i < usable_symbols; ++i) {
            sim::SymbolConfig symbol_config;
            symbol_config.symbol = kNames[i];
            symbol_config.initial_price = kPrices[i];
            config.symbols.push_back(symbol_config);
        }
    }

    std::cout << "Bots: " << config.noise_traders << " noise, " << config.momentum_traders << " momentum, " << config.market_makers << " market-maker -- each trading across all " << config.symbols.size() << " symbols (not pinned to one).\n";

    sim::Simulation simulation(config);
    std::string report = simulation.run();
    std::cout << "\n" << report << std::endl;

    return EXIT_SUCCESS;
}
