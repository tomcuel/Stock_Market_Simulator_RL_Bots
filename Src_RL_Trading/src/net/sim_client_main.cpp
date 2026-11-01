//=======================================================================
// sim_client.x: the client of the client-server architecture, two modes in one binary.
//
// Interactive / piped mode: type (or pipe) protocol commands, one per line, see `help`:
//   ./sim_client.x alice secret --register          # first time: creates the account
//   ./sim_client.x alice secret                     # next times: LOGIN
//   ./sim_client.x --resume <token>                 # reconnect with the token REGISTER/LOGIN printed
//   printf 'PORTFOLIO\nMARKET AAPL\n' | ./sim_client.x alice secret
//
// Bot mode: runs one trading strategy from bot.hpp against the server, through a WireGateway, so every decision the bot makes is a literal protocol command sent over the socket, the exact same thing a human would type
// It is the same strategy code simulation.x runs in-process
//   ./sim_client.x bot1 pw --register --bot trend --duration-sec 60
//   ./sim_client.x bot2 pw --register --bot ml --ml-model models/signal_model.csv
//
// Options (all optional):
//   --host H              server address (default 127.0.0.1)
//   --port P              server port (default 7878)
//   --register            create the account instead of logging in
//   --resume TOKEN        reconnect with a session token instead of username/password
//   --bot STRATEGY        noise | momentum | marketmaker | meanreversion | trend | twap | ml
//   --symbols S1 S2 ...   symbols the bot trades (default: every symbol the server has)
//   --duration-sec N      how long the bot trades (default 30)
//   --min-interval-ms N   shortest pause between two decisions (default 200)
//   --max-interval-ms N   longest pause between two decisions (default 1000)
//   --seed N              RNG seed (default 42)
//   --ml-model PATH       model for --bot ml (default models/signal_model.csv)
//   --ml-bar-ms N         --bot ml: one simulated trading day of N ms (default 1000)
//   --quiet               bot mode: don't print every ORDER / CANCEL and its answer
//=======================================================================
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "bot.hpp"
#include "net/client_connection.hpp"
#include "net/wire_gateway.hpp"
#include "signal_model.hpp"

namespace {

using sim::net::ClientConnection;

void print_usage(const char* program) {
    std::cout << "Usage:\n"
              << "  " << program << " <username> <password> [--register] [--host H] [--port P]\n"
              << "      interactive / piped mode: one protocol command per line, try 'help'\n"
              << "  " << program << " --resume <token> [--host H] [--port P]\n"
              << "      reconnect with the session token printed by REGISTER / LOGIN\n"
              << "  " << program << " <username> <password> --bot <strategy> [--register] [options]\n"
              << "      bot mode: runs a strategy from bot.hpp over the socket protocol\n"
              << "      strategies: noise momentum marketmaker meanreversion trend twap ml\n\n"
              << "Bot options: --symbols S1 S2 ... (default: all), --duration-sec N (30),\n"
              << "  --min-interval-ms N (200), --max-interval-ms N (1000), --seed N (42),\n"
              << "  --ml-model PATH (models/signal_model.csv), --ml-bar-ms N (1000), --quiet\n";
}

void print_help() {
    std::cout << "Commands:\n"
                 "  ORDER <BUY|SELL> <SYMBOL> <QTY> <MARKET|LIMIT|STOP|LIMIT_STOP> [PRICE=p] [TRIGGER=t]\n"
                 "        [TRIGGER_LOWER=l] [TRIGGER_UPPER=u] [EXPIRES=sec] [NOT_BEFORE=sec] [TIF=DAY|GTC]\n"
                 "  CANCEL <order_id> <SYMBOL>\n"
                 "  CANCEL_WAITING <order_id>\n"
                 "  CANCEL_BOOK <order_id> <SYMBOL> <BUY|SELL> <price>\n"
                 "  PORTFOLIO        cash, reserved_cash (engaged by open orders), available_cash, holdings\n"
                 "  MARKET <SYMBOL>\n"
                 "  SYMBOLS\n"
                 "  METRICS\n"
                 "  quit / exit\n";
}

// sends REGISTER / LOGIN / RESUME, prints the answer, and returns the client id on success
std::optional<sim::ClientId> authenticate(ClientConnection& connection, const std::string& command) {
    auto response = connection.send_command(command);
    if (!response) {
        std::cerr << "No response from the server during authentication\n";
        return std::nullopt;
    }
    std::cout << *response << "\n";
    if (response->rfind("OK", 0) != 0) {
        return std::nullopt;
    }
    auto pos = response->find("client_id=");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    try {
        return std::stoll(response->substr(pos + 10));
    } catch (...) {
        return std::nullopt;
    }
}

int run_interactive(ClientConnection& connection) {
    std::cout << "Connected. Type 'help' for the command list, 'quit' to disconnect.\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        if (line == "help") {
            print_help();
            continue;
        }
        if (line == "quit" || line == "exit") {
            connection.send_command("QUIT");
            break;
        }
        auto response = connection.send_command(line);
        if (!response) {
            std::cerr << "Disconnected from the server\n";
            return EXIT_FAILURE;
        }
        std::cout << *response << "\n";
    }
    return EXIT_SUCCESS;
}

struct BotSettings {
    std::string strategy;
    std::vector<std::string> symbols;
    int duration_sec{30};
    int min_interval_ms{200};
    int max_interval_ms{1000};
    unsigned seed{42};
    std::string ml_model{"models/signal_model.csv"};
    int ml_bar_ms{1000};
    bool quiet{false};
};

int run_bot(ClientConnection& connection, sim::ClientId client, const BotSettings& settings) {
    sim::net::WireGateway gateway(connection, client);
    if (!settings.quiet) {
        gateway.set_echo(&std::cout);
    }

    std::vector<std::string> symbols = settings.symbols.empty() ? gateway.symbols() : settings.symbols;
    if (symbols.empty()) {
        std::cerr << "No symbol to trade: the server reports none and --symbols wasn't given\n";
        return EXIT_FAILURE;
    }
    std::cout << "Strategy " << settings.strategy << " on " << symbols.size() << " symbol(s):";
    for (const auto& s : symbols) {
        std::cout << ' ' << s;
    }
    std::cout << "\n";

    std::shared_ptr<sim::SignalModel> model;
    if (settings.strategy == "ml") {
        model = std::make_shared<sim::SignalModel>();
        std::string error;
        if (!model->load(settings.ml_model, error)) {
            std::cerr << "Cannot run the ml bot: " << error << "\n";
            return EXIT_FAILURE;
        }
    }
    auto strategy = sim::make_strategy(settings.strategy, gateway, symbols, settings.seed, {}, model, std::chrono::milliseconds(settings.ml_bar_ms));
    if (!strategy) {
        std::cerr << "Unknown strategy '" << settings.strategy << "' (expected: noise momentum marketmaker meanreversion trend twap ml)\n";
        return EXIT_FAILURE;
    }

    std::mt19937 rng(settings.seed);
    std::uniform_int_distribution<int> pause(settings.min_interval_ms, std::max(settings.min_interval_ms, settings.max_interval_ms));
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(settings.duration_sec);
    while (std::chrono::steady_clock::now() < end) {
        strategy->step();
        if (!gateway.connected()) {
            std::cerr << "Disconnected from the server\n";
            return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pause(rng)));
    }
    std::cout << "Done: " << strategy->orders_placed() << " orders placed, " << strategy->orders_retracted() << " retracted\n";
    connection.send_command("QUIT");
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char* argv[]) {
    // if the server goes away mid-command, fail that command cleanly instead of dying on SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);

    std::string username, password, resume_token;
    std::string host = "127.0.0.1";
    int port = 7878;
    bool do_register = false;
    BotSettings bot;

    int i = 1;
    if (argc >= 3 && std::string(argv[1]).rfind("--", 0) != 0) {
        username = argv[1];
        password = argv[2];
        i = 3;
    }
    for (; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string{ return (i + 1 < argc) ? argv[++i] : ""; };
        if (arg == "--host") host = next();
        else if (arg == "--port") {
            port = std::atoi(next().c_str());
        }
        else if (arg == "--register") {
            do_register = true;
        }
        else if (arg == "--resume") {
            resume_token = next();
        }
        else if (arg == "--bot") {
            bot.strategy = next();
        }
        else if (arg == "--symbol") {
            bot.symbols = {next()};
        }
        else if (arg == "--symbols") {
            while (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                bot.symbols.push_back(argv[++i]);
            }
        } 
        else if (arg == "--duration-sec") {
            bot.duration_sec = std::atoi(next().c_str());
        }
        else if (arg == "--min-interval-ms") {
            bot.min_interval_ms = std::atoi(next().c_str());
        }
        else if (arg == "--max-interval-ms") {
            bot.max_interval_ms = std::atoi(next().c_str());
        }
        else if (arg == "--seed") {
            bot.seed = static_cast<unsigned>(std::atoi(next().c_str()));
        }
        else if (arg == "--ml-model") {
            bot.ml_model = next();
        }
        else if (arg == "--ml-bar-ms") {
            bot.ml_bar_ms = std::max(0, std::atoi(next().c_str()));
        }
        else if (arg == "--quiet") {
            bot.quiet = true;
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
    if (resume_token.empty() && (username.empty() || password.empty())) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    ClientConnection connection;
    if (!connection.connect(host, port)) {
        std::cerr << "Failed to connect to " << host << ":" << port << "\n";
        return EXIT_FAILURE;
    }
    std::string command = !resume_token.empty() ? "RESUME " + resume_token : (do_register ? "REGISTER " : "LOGIN ") + username + " " + password;
    auto client = authenticate(connection, command);
    if (!client) {
        return EXIT_FAILURE;
    }

    if (!bot.strategy.empty()) {
        return run_bot(connection, *client, bot);
    }
    return run_interactive(connection);
}
