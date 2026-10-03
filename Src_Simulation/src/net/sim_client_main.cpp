#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net/client_connection.hpp"

namespace {

using sim::net::ClientConnection;

void print_usage(const char* program) {
    std::cout << "Usage:\n"
              << "  " << program << " <username> <password> [--host H] [--port P] [--register]\n"
              << "      Interactive/piped mode: type (or pipe) commands, one per line: try 'help'\n"
              << "  " << program << " --resume <token> [--host H] [--port P]\n"
              << "      Reconnects using a session token from a previous REGISTER/LOGIN response without sending the password again\n"
              << "  " << program << " <username> <password> --bot <noise|momentum|marketmaker>\n"
              << "                [--symbols SYM1 SYM2 ...] [--host H] [--port P] [--register]\n"
              << "                [--min-interval-ms N] [--max-interval-ms N] [--duration-sec N] [--seed N]\n"
              << "      Bot mode: runs a strategy loop that sends real ORDER/MARKET commands over the socket on a timer \n"
              << "      Every decision is a literal command line, exactly like a human typing into interactive mode, not a direct in-process engine call\n";
}

void print_help() {
    std::cout <<
        "Commands:\n"
        "  ORDER <BUY|SELL> <SYMBOL> <QTY> <MARKET|LIMIT|STOP|LIMIT_STOP> [PRICE=p] [TRIGGER=t]\n"
        "        [TRIGGER_LOWER=l] [TRIGGER_UPPER=u] [EXPIRES=sec] [NOT_BEFORE=sec]\n"
        "  CANCEL <order_id> <SYMBOL>\n"
        "  CANCEL_WAITING <order_id>\n"
        "  CANCEL_BOOK <order_id> <SYMBOL> <BUY|SELL> <price>\n"
        "  PORTFOLIO\n"
        "  MARKET <SYMBOL>\n"
        "  SYMBOLS\n"
        "  METRICS\n"
        "  quit / exit\n";
}

bool authenticate(ClientConnection& conn, const std::string& username, const std::string& password, bool do_register) {
    std::ostringstream oss;
    oss << (do_register ? "REGISTER " : "LOGIN ") << username << " " << password;
    auto response = conn.send_command(oss.str());
    if (!response.has_value()) {
        std::cerr << "No response from server during authentication.\n";
        return false;
    }
    std::cout << *response << "\n";
    return response->rfind("OK", 0) == 0;
}

bool authenticate_resume(ClientConnection& conn, const std::string& token) {
    auto response = conn.send_command("RESUME " + token);
    if (!response.has_value()) {
        std::cerr << "No response from server during RESUME.\n";
        return false;
    }
    std::cout << *response << "\n";
    return response->rfind("OK", 0) == 0;
}

// pulls the value of "key=" out of a server response line like "OK MARKET AAPL last=150.2 bid=149.9"
std::optional<double> extract_field(const std::string& response, const std::string& key) {
    std::string needle = key + "=";
    auto pos = response.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    auto end = response.find(' ', pos);
    std::string value_str = response.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    try {
        return std::stod(value_str);
    } catch (...) {
        return std::nullopt;
    }
}

// queries the server for every symbol it has registered (used to auto-discover the tradable universe for a bot that wasn't given an explicit --symbols list)
std::vector<std::string> discover_symbols(ClientConnection& conn) {
    auto response = conn.send_command("SYMBOLS");
    std::vector<std::string> symbols;
    if (!response.has_value()) return symbols;
    // response looks like "OK SYMBOLS AAPL MSFT GOOG ..."
    std::istringstream iss(*response);
    std::string token;
    iss >> token >> token; // discard "OK" and "SYMBOLS"
    while (iss >> token) symbols.push_back(token);
    return symbols;
}

int run_interactive(ClientConnection& conn) {
    std::cout << "Connected. Type 'help' for the command list, 'quit' to disconnect.\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "help") {
            print_help();
            continue;
        }
        if (line == "quit" || line == "exit") {
            conn.send_command("QUIT");
            break;
        }
        auto response = conn.send_command(line);
        if (!response.has_value()) {
            std::cerr << "Disconnected from server.\n";
            return EXIT_FAILURE;
        }
        std::cout << *response << "\n";
    }
    return EXIT_SUCCESS;
}

// Builds and sends one ORDER command for the "noise trader" strategy: 
// random side, random kind across all four order types (so the simulation continuously exercises MARKET/LIMIT/STOP/LIMIT_STOP), random size, price offset around the last known price
void noise_bot_tick(ClientConnection& conn, const std::string& symbol, std::mt19937& rng, double last_price) {
    if (last_price <= 0.0) {
        return;
    }

    std::discrete_distribution<int> kind_dist({45, 35, 10, 10});
    std::bernoulli_distribution buy_or_sell(0.5);
    std::uniform_int_distribution<int> qty_dist(1, 10);
    std::uniform_real_distribution<double> offset_dist(-0.02, 0.02);

    std::string side = buy_or_sell(rng) ? "BUY" : "SELL";
    int qty = qty_dist(rng);

    std::ostringstream oss;
    oss << std::setprecision(15) << "ORDER " << side << " " << symbol << " " << qty << " ";

    switch (kind_dist(rng)) {
        case 0:
            oss << "MARKET";
            break;
        case 1: {
            double price = std::max(0.01, last_price * (1.0 + offset_dist(rng)));
            oss << "LIMIT PRICE=" << price;
            break;
        }
        case 2: {
            std::uniform_real_distribution<double> stop_offset_dist(0.02, 0.06);
            double direction = (side == "SELL") ? -1.0 : 1.0;
            double trigger = std::max(0.01, last_price * (1.0 + direction * stop_offset_dist(rng)));
            oss << "STOP TRIGGER=" << trigger;
            break;
        }
        default: {
            std::uniform_real_distribution<double> band_offset_dist(0.02, 0.05);
            double lower = std::max(0.01, last_price * (1.0 - band_offset_dist(rng)));
            double upper = last_price * (1.0 + band_offset_dist(rng));
            oss << "LIMIT_STOP PRICE=" << last_price << " TRIGGER_LOWER=" << lower << " TRIGGER_UPPER=" << upper;
            break;
        }
    }

    std::bernoulli_distribution has_expiry(0.25);
    if (has_expiry(rng)) {
        std::uniform_int_distribution<int> expiry_dist(5, 60);
        oss << " EXPIRES=" << expiry_dist(rng);
    }

    auto response = conn.send_command(oss.str());
    if (response.has_value()) std::cout << oss.str() << "  ->  " << *response << "\n";
}

// Momentum strategy: keeps a short rolling window of prices fetched over the wire
// (the bot process has no direct access to MarketData: it only knows what market responses tell it)
void momentum_bot_tick(ClientConnection& conn, const std::string& symbol, std::mt19937& rng, std::unordered_map<std::string, std::deque<double>>& price_history_by_symbol, double last_price) {
    if (last_price <= 0.0) {
        return;
    }
    auto& price_history = price_history_by_symbol[symbol];
    price_history.push_back(last_price);
    if (price_history.size() > 5) {
        price_history.pop_front();
    }
    if (price_history.size() < 2) {
        return;
    }

    double change = price_history.back() - price_history.front();
    if (change == 0.0) {
        return;
    }

    std::uniform_int_distribution<int> qty_dist(1, 5);
    int qty = qty_dist(rng);
    std::string side = (change > 0.0) ? "BUY" : "SELL";

    std::ostringstream order_oss;
    order_oss << std::setprecision(15) << "ORDER " << side << " " << symbol << " " << qty << " MARKET";
    auto response = conn.send_command(order_oss.str());
    if (response.has_value()) {
        std::cout << order_oss.str() << "  ->  " << *response << "\n";
    }

    std::uniform_real_distribution<double> stop_offset_dist(0.01, 0.03);
    double direction = (side == "BUY") ? -1.0 : 1.0;
    double trigger = std::max(0.01, last_price * (1.0 + direction * stop_offset_dist(rng)));
    std::string stop_side = (side == "BUY") ? "SELL" : "BUY";

    std::ostringstream stop_oss;
    stop_oss << std::setprecision(15) << "ORDER " << stop_side << " " << symbol << " " << qty << " STOP TRIGGER=" << trigger << " EXPIRES=120";
    auto stop_response = conn.send_command(stop_oss.str());
    if (stop_response.has_value()) {
        std::cout << stop_oss.str() << "  ->  " << *stop_response << "\n";
    }
}

// Market maker: quotes a fresh bid/ask pair around the last price every tick, cancelling its own previous quotes first via CANCEL so the book doesn't accumulate an ever-growing pile of stale resting orders
// (on whichever symbol they were posted on: tracked in last_quote_symbol, since this bot now rotates across symbols too)
void market_maker_bot_tick(ClientConnection& conn, const std::string& symbol, std::mt19937& rng, double last_price, std::string& last_quote_symbol, long long& last_buy_order_id, long long& last_sell_order_id) {
    if (last_buy_order_id != 0) {
        auto cancel_response = conn.send_command("CANCEL " + std::to_string(last_buy_order_id) + " " + last_quote_symbol);
        if (cancel_response.has_value()) {
            std::cout << "CANCEL " << last_buy_order_id << "  ->  " << *cancel_response << "\n";
        }
        last_buy_order_id = 0;
    }
    if (last_sell_order_id != 0) {
        auto cancel_response = conn.send_command("CANCEL " + std::to_string(last_sell_order_id) + " " + last_quote_symbol);
        if (cancel_response.has_value()) {
            std::cout << "CANCEL " << last_sell_order_id << "  ->  " << *cancel_response << "\n";
        }
        last_sell_order_id = 0;
    }
    last_quote_symbol = symbol;

    if (last_price <= 0.0) {
        return;
    }

    std::uniform_int_distribution<int> qty_dist(5, 20);
    double half_spread = last_price * 0.01 / 2.0;
    double bid = std::max(0.01, last_price - half_spread);
    double ask = last_price + half_spread;

    std::ostringstream buy_oss;
    buy_oss << std::setprecision(15) << "ORDER BUY " << symbol << " " << qty_dist(rng) << " LIMIT PRICE=" << bid;
    auto buy_response = conn.send_command(buy_oss.str());
    if (buy_response.has_value()) {
        std::cout << buy_oss.str() << "  ->  " << *buy_response << "\n";
        if (auto id = extract_field(*buy_response, "order_id")) {
            last_buy_order_id = static_cast<long long>(*id);
        }
    }

    std::ostringstream sell_oss;
    sell_oss << std::setprecision(15) << "ORDER SELL " << symbol << " " << qty_dist(rng) << " LIMIT PRICE=" << ask;
    auto sell_response = conn.send_command(sell_oss.str());
    if (sell_response.has_value()) {
        std::cout << sell_oss.str() << "  ->  " << *sell_response << "\n";
        if (auto id = extract_field(*sell_response, "order_id")) {
            last_sell_order_id = static_cast<long long>(*id);
        }
    }
}

int run_bot(ClientConnection& conn, const std::string& strategy, std::vector<std::string> symbols, unsigned seed,
            int min_interval_ms, int max_interval_ms, int duration_sec) {
    if (symbols.empty()) {
        symbols = discover_symbols(conn);
        if (symbols.empty()) {
            std::cerr << "No --symbols given and the server reports none registered either: nothing to trade\n";
            return EXIT_FAILURE;
        }
        std::cout << "Auto-discovered " << symbols.size() << " symbol(s) from the server: ";
        for (const auto& s : symbols) {
            std::cout << s << " ";
        }
        std::cout << "\n";
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> jitter(min_interval_ms, max_interval_ms);
    std::uniform_int_distribution<std::size_t> symbol_dist(0, symbols.size() - 1);
    std::unordered_map<std::string, std::deque<double>> price_history_by_symbol;
    std::string last_quote_symbol;
    long long last_buy_order_id = 0, last_sell_order_id = 0;

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < duration_sec) {
        // pick a fresh symbol every tick: this bot trades across the whole given market, it is not pinned to one instrument (see print_usage()'s explanation of why that matters)
        const std::string& symbol = symbols[symbol_dist(rng)];

        auto market_response = conn.send_command("MARKET " + symbol);
        if (!market_response.has_value()) {
            std::cerr << "Disconnected from server.\n";
            return EXIT_FAILURE;
        }
        double last_price = extract_field(*market_response, "last").value_or(0.0);

        if (strategy == "noise") {
            noise_bot_tick(conn, symbol, rng, last_price);
        } 
        else if (strategy == "momentum") {
            momentum_bot_tick(conn, symbol, rng, price_history_by_symbol, last_price);
        } 
        else if (strategy == "marketmaker") {
            market_maker_bot_tick(conn, symbol, rng, last_price, last_quote_symbol, last_buy_order_id, last_sell_order_id);
        } 
        else {
            std::cerr << "Unknown strategy: " << strategy << "\n";
            return EXIT_FAILURE;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(jitter(rng)));
    }

    conn.send_command("QUIT");
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char* argv[]) {
    // if the server goes away mid-command, fail that command cleanly instead of dying on SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
    // --resume <token> is a distinct calling convention (no positional username/password), so detect it first by scanning all args rather than assuming argv[1]/argv[2] exist
    std::string resume_token;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--resume" && i + 1 < argc) {
            resume_token = argv[i + 1];
            break;
        }
    }

    int first_option_index;
    std::string username, password;
    if (!resume_token.empty()) {
        first_option_index = 1; // no positional username/password to skip over
    } 
    else {
        if (argc < 3) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        username = argv[1];
        password = argv[2];
        first_option_index = 3;
    }

    std::string host = "127.0.0.1";
    int port = 7878;
    bool do_register = false;
    std::string bot_strategy;
    std::vector<std::string> bot_symbols; // empty = auto-discover every symbol the server has
    int min_interval_ms = 200;
    int max_interval_ms = 1000;
    int duration_sec = 30;
    unsigned seed = 42;

    for (int i = first_option_index; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };

        if (arg == "--host") {
            host = next();
        }
        else if (arg == "--port") {
            port = std::atoi(next().c_str());
        }
        else if (arg == "--register") {
            do_register = true;
        }
        else if (arg == "--resume") {
            next(); // already consumed by the scan above, just skip its value here
        }
        else if (arg == "--bot") {
            bot_strategy = next();
        }
        else if (arg == "--symbol") {
            bot_symbols = {next()}; // single-symbol convenience alias
        }
        else if (arg == "--symbols") {
            // consumes every following token up to the next `--flag` (or end of args)
            while (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                bot_symbols.push_back(argv[++i]);
            }
        }
        else if (arg == "--min-interval-ms") {
            min_interval_ms = std::atoi(next().c_str());
        }
        else if (arg == "--max-interval-ms") {
            max_interval_ms = std::atoi(next().c_str());
        }
        else if (arg == "--duration-sec") {
            duration_sec = std::atoi(next().c_str());
        }
        else if (arg == "--seed") {
            seed = static_cast<unsigned>(std::atoi(next().c_str()));
        }
        else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    ClientConnection conn;
    if (!conn.connect(host, port)) {
        std::cerr << "Failed to connect to " << host << ":" << port << "\n";
        return EXIT_FAILURE;
    }

    bool authenticated = resume_token.empty() ? authenticate(conn, username, password, do_register) : authenticate_resume(conn, resume_token);
    if (!authenticated) {
        return EXIT_FAILURE;
    }

    if (!bot_strategy.empty()) {
        return run_bot(conn, bot_strategy, bot_symbols, seed, min_interval_ms, max_interval_ms, duration_sec);
    }
    return run_interactive(conn);
}
