// Full-stack integration tests: a real SimulationServer, bound to a real (loopback) TCP port, hit by multiple real ClientConnections from multiple threads. 
// This is the test that exercises the entire concurrency story end-to-end at once the same way actual bots and clients do, rather than calling engine methods directly in-process like the rest of the suite does
// accept_loop's thread-per-connection model, every mutex in MatchingEngine/OrderBook/OrderRegistry/ClientDirectory, and the framing protocol
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "market_data.hpp"
#include "matching_engine.hpp"
#include "bot.hpp"
#include "net/client_connection.hpp"
#include "net/wire_gateway.hpp"
#include "test_helpers.hpp"
#include "net/protocol.hpp"
#include "net/simulation_server.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"

using namespace sim;
using namespace sim::net;

namespace {

// ports differ per test (rather than being reused) so tests can run even if a previous test's  server hasn't fully released its port yet (TIME_WAIT), without adding artificial sleeps
std::atomic<int> g_next_port{17800};
int next_test_port() {return g_next_port.fetch_add(1);}

struct TestServer {
    NotificationBus bus;
    MatchingEngine engine{bus};
    MarketData market_data{bus};
    SimulationServer server;
    int port;

    explicit TestServer(std::vector<std::pair<Symbol, Price>> symbols, ServerConfig config_overrides): server(make_config(config_overrides), bus, engine, market_data), port(config_overrides.port) {
        for (const auto& [symbol, price] : symbols) {
            engine.register_symbol(symbol, price);
        }
    }

    ~TestServer() {server.stop();}

    bool start() {return server.start();}

    static ServerConfig make_config(ServerConfig overrides) {
        overrides.watcher_poll_interval = std::chrono::milliseconds(20); // fast, so tests don't wait long
        return overrides;
    }
};

std::optional<std::string> register_and_get_response(ClientConnection& conn, const std::string& username, const std::string& password) {
    return conn.send_command("REGISTER " + username + " " + password);
}

} // namespace

TEST_CASE(server_rejects_commands_before_authentication) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    ClientConnection conn;
    CHECK(conn.connect("127.0.0.1", test_server.port));

    auto response = conn.send_command("PORTFOLIO");
    CHECK(response.has_value());
    CHECK(response->rfind("ERR NOT_AUTHENTICATED", 0) == 0);
}

TEST_CASE(server_register_then_order_then_portfolio_round_trip) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    ClientConnection conn;
    CHECK(conn.connect("127.0.0.1", test_server.port));

    auto register_response = register_and_get_response(conn, "alice", "pw");
    CHECK(register_response.has_value());
    CHECK(register_response->rfind("OK REGISTERED", 0) == 0);

    auto portfolio_response = conn.send_command("PORTFOLIO");
    CHECK(portfolio_response.has_value());
    CHECK(portfolio_response->find("AAPL=50000") != std::string::npos);

    auto order_response = conn.send_command("ORDER SELL AAPL 5 LIMIT PRICE=101");
    CHECK(order_response.has_value());
    CHECK(order_response->find("status=RESTING") != std::string::npos);
}

TEST_CASE(server_wrong_password_is_rejected_over_the_wire) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    {
        ClientConnection conn;
        conn.connect("127.0.0.1", test_server.port);
        register_and_get_response(conn, "alice", "correct_password");
    }

    ClientConnection conn2;
    CHECK(conn2.connect("127.0.0.1", test_server.port));
    auto response = conn2.send_command("LOGIN alice wrong_password");
    CHECK(response.has_value());
    CHECK_EQ(*response, std::string("ERR WRONG_PASSWORD"));
}

// Regression tests for a real crash found in production use: a client sending a command that references a symbol the server never registered (e.g. a bot's hardcoded symbol list not matching what the server actually seeded from Data/)
// These tests hit exactly the commands that crashed it, then prove the server is still alive and answering other clients afterward
TEST_CASE(market_command_for_unknown_symbol_returns_error_not_a_crash) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    ClientConnection conn;
    CHECK(conn.connect("127.0.0.1", test_server.port));
    register_and_get_response(conn, "alice", "pw");

    auto response = conn.send_command("MARKET NOT_A_REAL_SYMBOL");
    CHECK(response.has_value()); // the server must still be alive to answer at all
    CHECK(response->rfind("OK", 0) == 0); // an empty/zeroed snapshot, not an error: see snapshot()'s doc comment
}

TEST_CASE(cancel_commands_for_unknown_symbol_return_not_found_not_a_crash) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    ClientConnection conn;
    CHECK(conn.connect("127.0.0.1", test_server.port));
    register_and_get_response(conn, "alice", "pw");

    auto cancel_response = conn.send_command("CANCEL 1 NOT_A_REAL_SYMBOL");
    CHECK(cancel_response.has_value());
    CHECK_EQ(*cancel_response, std::string("ERR NOT_FOUND"));

    auto cancel_book_response = conn.send_command("CANCEL_BOOK 1 NOT_A_REAL_SYMBOL BUY 100");
    CHECK(cancel_book_response.has_value());
    CHECK_EQ(*cancel_book_response, std::string("ERR NOT_FOUND"));
}

TEST_CASE(server_survives_a_bad_client_and_keeps_serving_other_clients) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    // client A sends exactly what crashed the server before this fix
    ClientConnection bad_client;
    CHECK(bad_client.connect("127.0.0.1", test_server.port));
    register_and_get_response(bad_client, "bad_client", "pw");
    bad_client.send_command("MARKET DOES_NOT_EXIST");
    bad_client.send_command("CANCEL 999 ALSO_DOES_NOT_EXIST");

    // client B, on a completely separate connection, must still be served normally afterward
    ClientConnection good_client;
    CHECK(good_client.connect("127.0.0.1", test_server.port));
    auto response = register_and_get_response(good_client, "good_client", "pw");
    CHECK(response.has_value());
    CHECK(response->rfind("OK REGISTERED", 0) == 0);

    auto portfolio_response = good_client.send_command("PORTFOLIO");
    CHECK(portfolio_response.has_value());
    CHECK(portfolio_response->find("AAPL=50000") != std::string::npos);
}

// Regression test for a real crash: a client that disappears while the server is still replying
// used to kill the whole server with SIGPIPE (the default action of writing to a dead socket)
TEST_CASE(server_survives_clients_that_vanish_mid_reply) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    for (int round = 0; round < 20; ++round) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(test_server.port));
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        disable_sigpipe(fd);
        send_framed(fd, "REGISTER ghost_" + std::to_string(round) + " pw");
        for (int i = 0; i < 50; ++i) send_framed(fd, "MARKET AAPL"); // replies never read
        linger abort_on_close{1, 0}; // close with RST: the server's next write hits a dead peer
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &abort_on_close, sizeof(abort_on_close));
        ::close(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the server hit the dead sockets

    // the server must still be alive and serving everyone else normally
    ClientConnection survivor;
    CHECK(survivor.connect("127.0.0.1", test_server.port));
    auto response = register_and_get_response(survivor, "survivor", "pw");
    CHECK(response.has_value());
    CHECK(response->rfind("OK REGISTERED", 0) == 0);
}

TEST_CASE(server_stop_gracefully_stops_all_background_threads) {
    ServerConfig config;
    config.port = next_test_port();
    auto test_server = std::make_unique<TestServer>(std::vector<std::pair<Symbol, Price>>{{"AAPL", 100.0}}, config);
    CHECK(test_server->start());

    ClientConnection conn;
    CHECK(conn.connect("127.0.0.1", test_server->port));
    register_and_get_response(conn, "alice", "pw");

    // destroying the TestServer calls SimulationServer::stop(), which must return in bounded time (not hang waiting on the accept loop or watcher thread)
    // if this test ever hangs, that's the regression it's designed to catch
    auto start = std::chrono::steady_clock::now();
    test_server.reset();
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(5));
}

// The core "does everything survive real concurrent socket load" test: many real client connections, each on its own thread, hammering ORDER through the actual wire protocol simultaneously
// Verifies the same fundamental invariant as test_concurrency.cpp's in-process version (total cash and shares are conserved) but this time through the full stack: 
//framing, per-connection threads, command parsing, and every engine mutex, all under contention at once
TEST_CASE(concurrent_clients_over_real_sockets_conserve_cash_and_shares) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    const int num_clients = 15;
    const int orders_per_client = 40;
    std::vector<std::thread> threads;
    std::vector<ClientId> client_ids(num_clients, 0);
    // each account's grant, read right after REGISTER: the balanced grant is computed at the price of the moment, and the other clients are already trading, so it is not exactly 100 for everyone
    std::vector<double> granted_cash(num_clients, 0.0);
    std::vector<Quantity> granted_shares(num_clients, 0);
    std::atomic<int> connect_failures{0};

    for (int t = 0; t < num_clients; ++t) {
        threads.emplace_back([&, t] {
            ClientConnection conn;
            if (!conn.connect("127.0.0.1", test_server.port)) {
                connect_failures.fetch_add(1);
                return;
            }
            auto response = register_and_get_response(conn, "client_" + std::to_string(t), "pw");
            if (!response.has_value()) {
                return;
            }

            auto id_pos = response->find("client_id=");
            if (id_pos != std::string::npos) {
                client_ids[t] = std::stoll(response->substr(id_pos + 10));
            }

            // no order yet, so nobody else's trade can have touched this account: this is exactly its grant
            auto portfolio = conn.send_command("PORTFOLIO");
            if (portfolio) {
                auto value_of = [&](const std::string& key) {
                    auto pos = portfolio->find(" " + key + "=");
                    return pos == std::string::npos ? std::string("0") : portfolio->substr(pos + key.size() + 2, portfolio->find(' ', pos + 1) - pos - key.size() - 2);
                };
                granted_cash[t] = std::stod(value_of("cash"));
                granted_shares[t] = std::stoll(value_of("AAPL"));
            }

            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> qty_dist(1, 3);
            std::bernoulli_distribution side_dist(0.5);
            std::uniform_real_distribution<double> price_dist(95.0, 105.0);
            std::bernoulli_distribution kind_dist(0.4);

            for (int i = 0; i < orders_per_client; ++i) {
                std::string side = side_dist(rng) ? "BUY" : "SELL";
                std::ostringstream oss;
                if (kind_dist(rng)) {
                    oss << "ORDER " << side << " AAPL " << qty_dist(rng) << " MARKET";
                } 
                else {
                    oss << std::setprecision(10) << "ORDER " << side << " AAPL " << qty_dist(rng) << " LIMIT PRICE=" << price_dist(rng);
                }
                conn.send_command(oss.str());
            }
            conn.send_command("QUIT");
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    CHECK_EQ(connect_failures.load(), 0);

    double total_cash = 0.0;
    Quantity total_shares = 0;
    int successfully_registered = 0;
    for (ClientId id : client_ids) {
        if (id == 0) {
            continue;
        }
        ++successfully_registered;
        Portfolio p = test_server.engine.portfolio_snapshot(id);
        total_cash += p.cash;
        total_shares += p.holding("AAPL");
    }

    CHECK_EQ(successfully_registered, num_clients);

    // every grant was balanced at its own price: half of the equity in cash, give or take the rounding of the share count (at most half a share, about 55 at AAPL's prices here)
    const double equity = ServerConfig{}.starting_equity;
    double total_granted_cash = 0.0;
    Quantity total_granted_shares = 0;
    for (int t = 0; t < num_clients; ++t){
        CHECK(std::abs(granted_cash[t] - equity * 0.5) < 1000.0);
        CHECK(granted_shares[t] > 0);
        total_granted_cash += granted_cash[t];
        total_granted_shares += granted_shares[t];
    }
    // trades only move cash and shares between clients: the totals are exactly what was granted
    CHECK_NEAR(total_cash, total_granted_cash, 1e-3);
    CHECK_EQ(total_shares, total_granted_shares);
}

// Many bots repeatedly hitting the SAME symbol's book concurrently to stress:
// - OrderRegistry (STOP orders queuing and being released by the watcher thread while more STOP orders keep arriving)
// - the dirty-symbol tracking path together, end-to-end over real sockets
TEST_CASE(concurrent_stop_orders_over_real_sockets_eventually_all_resolve) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    const int num_clients = 8;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_clients; ++t) {
        threads.emplace_back([&, t] {
            ClientConnection conn;
            if (!conn.connect("127.0.0.1", test_server.port)) {
                return;
            }
            register_and_get_response(conn, "stopbot_" + std::to_string(t), "pw");

            // place a STOP order that will never trigger (way out of band, stays in the waiting registry) 
            // and one that's already within its release band at submission time (so it releases immediately, before ever touching the waiting registry)
            // exercising both "stays queued" and "the immediate-release path" under concurrent registry access
            conn.send_command("ORDER SELL AAPL 1 STOP TRIGGER=1");   // never triggers (price starts at 100)
            conn.send_command("ORDER SELL AAPL 1 STOP TRIGGER=200"); // releases immediately (100 <= 200)
            conn.send_command("QUIT");
        });
    }
    for (auto& th : threads) th.join();

    // give the watcher thread a moment to run its release scan
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // every "triggers immediately" order (band upper=200, satisfied by the starting price of 100) should have been released and removed from the waiting registry; only the "never triggers" ones should remain
    CHECK_EQ(test_server.engine.waiting_order_count(), std::size_t{num_clients});
}


namespace {

// client id from "OK REGISTERED client_id=3 token=..."
ClientId client_id_of(const std::optional<std::string>& response) {
    if (!response) {
        return 0;
    }
    auto pos = response->find("client_id=");
    return pos == std::string::npos ? 0 : std::stoll(response->substr(pos + 10));
}

// order id from "OK ORDER order_id=12 status=..."
std::string order_id_of(const std::optional<std::string>& response) {
    auto pos = response->find("order_id=") + 9;
    return response->substr(pos, response->find(' ', pos) - pos);
}

} // namespace

// Regression test: CANCEL, CANCEL_BOOK and CANCEL_WAITING used to remove any order by id, so a client could retract another client's orders by guessing their sequential ids
// Someone else's order must now look exactly like a missing one
TEST_CASE(a_client_cannot_cancel_another_clients_orders) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());

    ClientConnection alice, mallory;
    CHECK(alice.connect("127.0.0.1", test_server.port));
    CHECK(mallory.connect("127.0.0.1", test_server.port));
    register_and_get_response(alice, "alice", "pw");
    register_and_get_response(mallory, "mallory", "pw");

    std::string limit_id = order_id_of(alice.send_command("ORDER BUY AAPL 5 LIMIT PRICE=90"));
    std::string stop_id = order_id_of(alice.send_command("ORDER SELL AAPL 5 STOP TRIGGER=80"));

    CHECK_EQ(*mallory.send_command("CANCEL " + limit_id + " AAPL"), std::string("ERR NOT_FOUND"));
    CHECK_EQ(*mallory.send_command("CANCEL_BOOK " + limit_id + " AAPL BUY 90"), std::string("ERR NOT_FOUND"));
    CHECK_EQ(*mallory.send_command("CANCEL_WAITING " + stop_id), std::string("ERR NOT_FOUND"));
    CHECK_EQ(*mallory.send_command("CANCEL " + stop_id + " AAPL"), std::string("ERR NOT_FOUND"));
    CHECK_EQ(test_server.engine.resting_orders("AAPL").size(), std::size_t{1}); // untouched
    CHECK_EQ(test_server.engine.waiting_orders().size(), std::size_t{1});

    CHECK_EQ(*alice.send_command("CANCEL " + limit_id + " AAPL"), std::string("OK CANCELLED")); // the owner can
    CHECK_EQ(*alice.send_command("CANCEL " + stop_id + " AAPL"), std::string("OK CANCELLED"));
}

// The same strategy code that simulation.x runs in-process must run unchanged over the socket protocol through a WireGateway (this is what sim_client.x --bot does)
TEST_CASE(strategies_run_unchanged_over_the_wire_gateway) {
    ServerConfig config;
    config.port = next_test_port();
    TestServer test_server({{"AAPL", 100.0}, {"MSFT", 200.0}}, config);
    CHECK(test_server.start());

    ClientConnection maker_connection, noise_connection;
    CHECK(maker_connection.connect("127.0.0.1", test_server.port));
    CHECK(noise_connection.connect("127.0.0.1", test_server.port));
    ClientId maker_id = client_id_of(register_and_get_response(maker_connection, "maker", "pw"));
    ClientId noise_id = client_id_of(register_and_get_response(noise_connection, "noise", "pw"));
    CHECK(maker_id != 0 && noise_id != 0);

    WireGateway maker_gateway(maker_connection, maker_id);
    WireGateway noise_gateway(noise_connection, noise_id);
    auto symbols = maker_gateway.symbols();
    CHECK_EQ(symbols.size(), std::size_t{2});
    CHECK(maker_gateway.quote("AAPL").last > 0.0);
    // the balanced starting grant parsed from PORTFOLIO: half of the 10 million split between AAPL at 100 and MSFT at 200
    CHECK_EQ(maker_gateway.portfolio().holding("AAPL"), Quantity{25000});
    CHECK_EQ(maker_gateway.portfolio().holding("MSFT"), Quantity{12500});

    MarketMakerBot maker(maker_gateway, symbols, 1);
    NoiseTraderBot noise(noise_gateway, symbols, 2);
    for (int i = 0; i < 40; ++i){
        maker.step();
        noise.step();
    }
    CHECK(maker.orders_placed() > 0);
    CHECK(noise.orders_placed() > 0);
    CHECK(maker.orders_retracted() > 0); // CANCEL worked over the wire
    std::size_t maker_resting = 0;
    for (const auto& s : symbols){
        for (const auto& order : test_server.engine.resting_orders(s)){
            if (order.client == maker_id) {
                ++maker_resting;
            }
        }
    }
    CHECK(maker_resting <= 2); // one requoted pair at most, exactly like in-process
    CHECK(maker_gateway.portfolio().reserved_cash >= 0.0);
    CHECK_EQ(WireGateway::order_command(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 3, 99.5)), std::string("ORDER BUY AAPL 3 LIMIT PRICE=99.5"));
}

TEST_CASE(server_stops_at_once_even_with_a_long_save_interval) {
    // the background loops used to sleep their whole interval: a stop waited for the end of the persistence thread's
    // current 30 s sleep (7.6 s in a real run), now they wake at once
    ServerConfig config;
    config.port = next_test_port();
    config.persistence_path = (nrt::fresh_output_dir("server_stop") / "snapshot.txt").string();
    config.persistence_save_interval = std::chrono::seconds(30);
    TestServer test_server({{"AAPL", 100.0}}, config);
    CHECK(test_server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // every background thread is now inside its wait
    auto started = std::chrono::steady_clock::now();
    test_server.server.stop();
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 2000);
    CHECK(std::filesystem::exists(config.persistence_path)); // and the final snapshot is still written
}
