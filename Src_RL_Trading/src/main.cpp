//=======================================================================
// simulation.x: in-process market simulation (no sockets), in trading days (see market_session.hpp):
// a MatchingEngine plus a pool of rule-based bots (noise, momentum, market makers...) that each trade across all symbols,
// and with --rl-port, the RL agents served by the Python policy server, each one trading as its own RLBots
// Prints a text report at the end and with --output-dir, writes CSV files for plotting
//
// Quick start (from Src_RL_Trading/, after `make`):
//   ./simulation.x                                    # 3 symbols, 60 trading days of 4 s, prints a report, writes no files
//   ./simulation.x --days 5 --output-dir output/my_run   # 5 days, plus metrics + market report CSVs:
//                                                     #   output/my_run/metrics_simu.csv, daily_bars_simu.csv
//                                                     #   output/my_run/{summary,symbols,trades,...}_simu.csv
//   ./simulation.x --symbols 8 --days 250 --noise 60 --momentum 20 --marketmakers 12 --output-dir output/big_run : a much bigger run
// The easiest way to run it with logs + plots is the launcher: ./launch.sh simu (see its header)
//
// Options (all optional; defaults give a working run):
//   --symbols N              symbols to simulate (default 3), taken from --data-seed when available, otherwise from a built-in list of 8 demo tickers
//   --days N                 trading days (default 60, at least 1): each day collects orders for the opening auction, opens,
//                            trades, collects orders for the closing auction, closes (DAY orders expire) and stays closed
//                            overnight, see market_session.hpp. The run lasts N x (day + overnight) and writes
//                            daily_bars_simu.csv, the RL agents decide once per day
//   --day-ms N               one trading day (default 4000: 60 days in 4 minutes)
//   --pre-open-ms N          its first part, orders collected for the opening auction (default 10% of the day)
//   --pre-close-ms N         its last part, orders collected for the closing auction (default 10% of the day)
//   --overnight-ms N         the market closed between two days (default 100)
//   --overnight-gap-vol X    standard deviation of the overnight log return, overnight news (default 0: the next day opens at the close)
//   --noise N                noise-trader bots (default 15)
//   --momentum N             momentum bots (default 5)
//   --marketmakers N         market-maker bots (default 4)
//   --meanreversion N        mean-reversion bots (default 4)
//   --trend N                trend-following bots (default 4)
//   --twap N                 TWAP execution bots (default 2)
//   --value N                value investors (default 0): every symbol then gets a fundamental value, a random walk on the ticker's
//                            real volatility, part of it arriving overnight (the gap at the open), see fundamentals.hpp
//   --maker-half-spread X    the market makers quote mid -/+ X (default 0.005: a 1% quoted spread)
//   --overnight-share X      with --value: the share of the daily variance that arrives overnight (default 0.35)
//   --fundamental-drift X    with --value: the values' drift per day (default 0.0005: +5 bp, the real tickers' mean)
//   --rl-port N              trade the RL agents served on this port (python -m rl_agents.serve, default 0: none)
//   --rl-host H              the policy server's address (default 127.0.0.1)
//   --rl-bots N              bots per served agent (default 2)
//   --rl-risk-fraction X     an RL bot's risk per trade, the share of the equity its stop-loss loses (default 0.002)
//   --bot-equity X           starting value of every bot's account (default 10000000)
//   --cash-fraction F        share of it held as cash, the rest split equally by value across the symbols (default 0.5)
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
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "logger.hpp"
#include "market_seed.hpp"
#include "paths.hpp"
#include "simulation.hpp"

namespace {

const char* kDefaultDataSeedPath = "../Data/Datasets/processed/latest_snapshot.csv";

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "In-process market simulation in trading days. With no options: 3 symbols, 60 days of 4 s, text report only.\n\n"
              << "  --symbols N              symbols to simulate (default 3)\n"
              << "  --days N                 trading days: auctions, continuous trading, night (default 60, at least 1)\n"
              << "  --day-ms N               one trading day (default 4000)\n"
              << "  --pre-open-ms N          its first part, orders collected for the opening auction (default 10% of the day)\n"
              << "  --pre-close-ms N         its last part, orders collected for the closing auction (default 10% of the day)\n"
              << "  --overnight-ms N         closed between two days (default 100)\n"
              << "  --overnight-gap-vol X    std of the overnight log return, overnight news (default 0)\n"
              << "  --noise N                noise-trader bots, trading across ALL symbols (default 15)\n"
              << "  --momentum N             momentum bots (default 5)\n"
              << "  --marketmakers N         market-maker bots (default 4)\n"
              << "  --meanreversion N        mean-reversion bots (default 4)\n"
              << "  --trend N                trend-following bots (default 4)\n"
              << "  --twap N                 TWAP execution bots (default 2)\n"
              << "  --value N                value investors, trading toward each symbol's fundamental value (default 0)\n"
              << "  --maker-half-spread X    market makers quote mid -/+ X (default 0.005)\n"
              << "  --overnight-share X      with --value: share of the daily variance arriving overnight (default 0.35)\n"
              << "  --fundamental-drift X    with --value: drift of the values per day (default 0.0005)\n"
              << "  --rl-port N              trade the RL agents served on this port (python -m rl_agents.serve)\n"
              << "  --rl-host H              policy server address (default 127.0.0.1)\n"
              << "  --rl-bots N              bots per served agent (default 2)\n"
              << "  --rl-risk-fraction X     an RL bot's risk per trade, share of equity (default 0.002)\n"
              << "  --bot-equity X           starting value of every bot's account (default 10000000)\n"
              << "  --cash-fraction F        share of it in cash, the rest split equally by value across symbols (default 0.5)\n"
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
    unsigned seed = 42;
    std::string log_level = "INFO";
    std::string data_seed_path = kDefaultDataSeedPath;
    bool use_data_seed = true;
    int noise_traders = 15;
    int momentum_traders = 5;
    int market_makers = 4;
    int mean_reversion = 4;
    int trend = 4;
    int twap = 2;
    int value = 0;
    double overnight_share = 0.35;
    double maker_half_spread = sim::MarketMakerBot::kHalfSpread;
    double fundamental_drift = 0.0005;
    int days = 60;
    int day_ms = 4000;
    int pre_open_ms = -1;  // 10% of the day
    int pre_close_ms = -1; // 10% of the day
    int overnight_ms = 100;
    double overnight_gap_vol = 0.0;
    std::string rl_host = "127.0.0.1";
    int rl_port = 0;
    int rl_bots = 2;
    double rl_risk_fraction = sim::RLBot::kRiskFraction;
    std::string output_dir;
    bool write_metrics = true;
    bool write_report = true;
    int metrics_interval_ms = 1000;
    int report_interval_ms = 250;
    double bot_equity = 10000000.0;
    double cash_fraction = 0.5;

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
        else if (arg == "--meanreversion") {
            mean_reversion = std::max(0, std::atoi(next_value("--meanreversion").c_str()));
        } 
        else if (arg == "--trend") {
            trend = std::max(0, std::atoi(next_value("--trend").c_str()));
        } 
        else if (arg == "--twap") {
            twap = std::max(0, std::atoi(next_value("--twap").c_str()));
        } 
        else if (arg == "--value") {
            value = std::max(0, std::atoi(next_value("--value").c_str()));
        }
        else if (arg == "--maker-half-spread") {
            maker_half_spread = std::atof(next_value("--maker-half-spread").c_str());
        }
        else if (arg == "--overnight-share") {
            overnight_share = std::clamp(std::atof(next_value("--overnight-share").c_str()), 0.0, 1.0);
        }
        else if (arg == "--fundamental-drift") {
            fundamental_drift = std::atof(next_value("--fundamental-drift").c_str());
        }
        else if (arg == "--days") {
            days = std::atoi(next_value("--days").c_str());
            if (days < 1) {
                std::cerr << "--days must be at least 1: trading days are the only mode\n";
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--day-ms") {
            day_ms = std::max(100, std::atoi(next_value("--day-ms").c_str()));
        }
        else if (arg == "--pre-open-ms") {
            pre_open_ms = std::max(0, std::atoi(next_value("--pre-open-ms").c_str()));
        }
        else if (arg == "--pre-close-ms") {
            pre_close_ms = std::max(0, std::atoi(next_value("--pre-close-ms").c_str()));
        }
        else if (arg == "--overnight-ms") {
            overnight_ms = std::max(0, std::atoi(next_value("--overnight-ms").c_str()));
        }
        else if (arg == "--overnight-gap-vol") {
            overnight_gap_vol = std::max(0.0, std::atof(next_value("--overnight-gap-vol").c_str()));
        }
        else if (arg == "--rl-port") {
            rl_port = std::max(0, std::atoi(next_value("--rl-port").c_str()));
        }
        else if (arg == "--rl-host") {
            rl_host = next_value("--rl-host");
        }
        else if (arg == "--rl-bots") {
            rl_bots = std::max(0, std::atoi(next_value("--rl-bots").c_str()));
        }
        else if (arg == "--rl-risk-fraction") {
            rl_risk_fraction = std::clamp(std::atof(next_value("--rl-risk-fraction").c_str()), 0.0001, 0.05);
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
    config.noise_traders = noise_traders;
    config.momentum_traders = momentum_traders;
    config.market_makers = market_makers;
    config.mean_reversion_traders = mean_reversion;
    config.trend_followers = trend;
    config.twap_executors = twap;
    config.value_investors = value;
    config.maker_half_spread = maker_half_spread;
    config.fundamental_overnight_share = overnight_share;
    config.fundamental_drift = fundamental_drift;
    config.session.days = days;
    config.session.day = std::chrono::milliseconds(day_ms);
    config.session.pre_open = std::chrono::milliseconds(pre_open_ms >= 0 ? std::min(pre_open_ms, day_ms / 3) : day_ms / 10);
    config.session.pre_close = std::chrono::milliseconds(pre_close_ms >= 0 ? std::min(pre_close_ms, day_ms / 3) : day_ms / 10);
    config.session.overnight = std::chrono::milliseconds(overnight_ms);
    config.session.overnight_gap_vol = overnight_gap_vol;
    config.rl_host = rl_host;
    config.rl_port = rl_port;
    config.rl_bots_per_model = rl_bots;
    config.rl_risk_fraction = rl_risk_fraction;
    config.bot_equity = bot_equity;
    config.bot_cash_fraction = cash_fraction;
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
        std::cout << "Seeding " << data_symbols.size() << " symbols from " << sim::display_path(data_seed_path) << "\n";
        for (const auto& seed_symbol : data_symbols) {
            sim::SymbolConfig symbol_config;
            symbol_config.symbol = seed_symbol.symbol;
            symbol_config.initial_price = seed_symbol.last_price;
            symbol_config.volatility = (seed_symbol.rolling_volatility_20 > 0.0) ? seed_symbol.rolling_volatility_20 : 0.20;
            config.symbols.push_back(symbol_config);
        }
    } 
    else {
        std::cout << "No usable data seed found at " << sim::display_path(data_seed_path) << " -- using hardcoded demo symbols.\n" << "(Run Data/preprocess.py && Data/feature_engineering.py first for real market data.)\n";
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

    std::cout << "Bots: " << config.noise_traders << " noise, " << config.momentum_traders << " momentum, " << config.market_makers << " market maker, " << config.mean_reversion_traders << " mean reversion, " << config.trend_followers << " trend, " << config.twap_executors << " twap, " << config.value_investors << " value" << (config.rl_port > 0 ? ", plus the RL agents of the policy server" : "") << " (each trading across all " << config.symbols.size() << " symbols)\n";

    sim::Simulation simulation(config);
    std::string report = simulation.run();
    std::cout << "\n" << report << std::endl;

    return EXIT_SUCCESS;
}
