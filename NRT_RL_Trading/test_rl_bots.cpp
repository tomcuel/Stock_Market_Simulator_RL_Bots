//=======================================================================
// Tests for the C++ half of the Python <-> C++ bridge: the policy client, the market feed and the RL bots (RL_Agents'
// trained agents trading the simulated exchange's trading days, one decision per day, see rl_bot.hpp).
// A small fake policy server, written here in C++ with scripted answers, replaces the Python one: the NRT stays free of
// Python, and every decision rule of the bot can be driven exactly. The Python half is covered by python -m rl_agents.selftest
//=======================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "bot.hpp"
#include "rl_bot.hpp"
#include "market_feed.hpp"
#include "matching_engine.hpp"
#include "net/protocol.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "policy_client.hpp"
#include "simulation.hpp"
#include "test_helpers.hpp"

using namespace sim;

namespace {

// answers every framed request with answer(request), on a free loopback port
class FakePolicyServer {
public:
    explicit FakePolicyServer(std::function<std::string(const std::string&)> answer) : answer_(std::move(answer)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0; // any free port
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listen_fd_, 16);
        socklen_t length = sizeof(address);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        accept_thread_ = std::thread([this] { run(); });
    }

    ~FakePolicyServer() {
        stop();
    }

    int port() const { return port_; }

    void set_answer(std::function<std::string(const std::string&)> answer) {
        std::lock_guard<std::mutex> lock(mutex_);
        answer_ = std::move(answer);
    }

    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    // closes the listening socket and every open connection, like a server that went away
    void stop() {
        if (stopped_.exchange(true)) return;
        if (accept_thread_.joinable()) accept_thread_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int fd : connections_) ::shutdown(fd, SHUT_RDWR);
        }
        for (auto& t : connection_threads_) {
            if (t.joinable()) t.join();
        }
        ::close(listen_fd_);
    }

private:
    void run() {
        while (!stopped_.load()) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue; // wakes up regularly to notice stop()
            int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) continue;
            net::disable_sigpipe(client);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                connections_.push_back(client);
            }
            connection_threads_.emplace_back([this, client] {
                while (auto request = net::recv_framed(client)) {
                    std::string reply;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        requests_.push_back(*request);
                        reply = answer_(*request);
                    }
                    if (!net::send_framed(client, reply)) break;
                }
                ::close(client);
            });
        }
    }

    std::function<std::string(const std::string&)> answer_;
    int listen_fd_{-1};
    int port_{0};
    std::atomic<bool> stopped_{false};
    std::thread accept_thread_;
    std::vector<std::thread> connection_threads_;
    std::vector<int> connections_;
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

} // namespace

// ---------------------------------------------------------------- the RL bots (RLBot)
namespace {

// a server serving one RL agent, "bravo", whose ACT answer is *fields (changeable while the test runs)
std::function<std::string(const std::string&)> rl_server(std::shared_ptr<std::string> fields) {
    return [fields](const std::string& request) {
        if (request == "MODELS") return std::string("OK MODELS bravo");
        if (request == "INFO bravo") return std::string("OK INFO model=bravo agent=ppo lookback=21 train_seeds=301,302");
        if (request.rfind("OBSERVE ", 0) == 0) return std::string("OK OBSERVED symbol=AAPL n=1");
        if (request.rfind("ACT bravo ", 0) == 0) return "OK ACTION model=bravo symbol=AAPL " + *fields;
        return std::string("ERR UNKNOWN_COMMAND");
    };
}

// clients 1 and 2 quote 100.5 / 99.5 (client 2's bid id kept), client 3 is the RL bot, client 4 a spare trader
struct RLFixture {
    NotificationBus bus;
    MatchingEngine engine{bus};
    MarketSession session;
    std::shared_ptr<PolicyClient> client = std::make_shared<PolicyClient>();
    std::unique_ptr<InProcessGateway> gateway;
    std::unique_ptr<RLBot> bot;
    OrderId bid_id{0};

    explicit RLFixture(FakePolicyServer& server) {
        engine.register_symbol("AAPL", 100.0);
        for (ClientId id = 1; id <= 4; ++id) engine.fund_balanced_account(id, 10000000.0, 0.5);
        engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 20000, 100.5));
        bid_id = engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 20000, 99.5)).order_id;
        client->connect("127.0.0.1", server.port());
        gateway = std::make_unique<InProcessGateway>(engine, 3);
        gateway->track_fills();
        bot = std::make_unique<RLBot>(*gateway, std::vector<Symbol>{"AAPL"}, 1, std::unordered_map<Symbol, double>{}, client, *client->info("bravo"));
        bot->set_session(&session);
    }

    // one day for the bot: its decision in the pre-open (here the engine isn't in auction mode, the orders fill at once),
    // then the first tick after the open, which settles them and places the legs
    void trade_day(int day) {
        session.set(day, SessionPhase::PRE_OPEN);
        bot->step();
        bus.flush(); // the fills reach the gateway through the bus's thread
        session.set(day, SessionPhase::OPEN);
        bot->step();
    }

    Quantity holding() {return engine.portfolio_snapshot(3).holding("AAPL");}
    std::size_t open_orders_of_the_bot() {
        std::size_t n = 0;
        for (const auto& open : engine.open_orders()) n += open.order.client == 3 ? 1 : 0;
        return n;
    }
};

const std::string kRLLong = "position=1 direction=1 sl_atr=1 tp_r=2 atr=2 close=100";
const std::string kRLFlat = "position=0 direction=0 sl_atr=1 tp_r=2 atr=2 close=100";

} // namespace

TEST_CASE(rl_answers_carry_the_stop_the_target_and_the_atr) {
    auto a = PolicyClient::parse_action("OK ACTION model=bravo symbol=AAPL position=-1 direction=-1 sl_atr=1.5 tp_r=3 atr=2.25");
    CHECK(a.ok && a.position == -1);
    CHECK_NEAR(a.sl_atr, 1.5, 1e-12);
    CHECK_NEAR(a.tp_r, 3.0, 1e-12);
    CHECK_NEAR(a.atr, 2.25, 1e-12);
}

TEST_CASE(rl_bot_enters_at_the_open_with_a_gtc_stop_and_target) {
    auto fields = std::make_shared<std::string>(kRLLong);
    FakePolicyServer server(rl_server(fields));
    RLFixture f(server);
    Quantity base = f.holding();
    f.trade_day(0);
    // risk 0.2% of 10M on a 2-point stop = 10000 shares, capped at 0.5% of the equity: 500 shares (at 100, the last price)
    CHECK_EQ(f.holding() - base, Quantity{500});
    CHECK_EQ(f.bot->open_direction("AAPL"), 1);
    bool stop = false, target = false;
    for (const auto& open : f.engine.open_orders()) {
        if (open.order.client != 3) continue;
        CHECK(open.order.time_in_force == TimeInForce::GTC);
        stop = stop || (open.waiting && open.order.kind == OrderKind::STOP && std::abs(open.order.release_upper - 98.5) < 1e-6);
        target = target || (!open.waiting && open.order.kind == OrderKind::LIMIT && std::abs(open.order.price - 104.5) < 1e-6);
    }
    CHECK(stop && target);
    CHECK_EQ(server.requests().back(), std::string("ACT bravo AAPL dir=0 entry=0 sl=0 tp=0 days=0 tpr=0"));
    CHECK(f.bot->trades().empty());
    f.bot->step(); // the same day: no second decision
    CHECK_EQ(f.holding() - base, Quantity{500});
}

TEST_CASE(rl_bot_stop_loss_fills_and_cancels_the_target) {
    auto fields = std::make_shared<std::string>(kRLLong);
    FakePolicyServer server(rl_server(fields));
    RLFixture f(server);
    Quantity base = f.holding();
    f.trade_day(0);
    // the market falls through the stop (98.5): client 2's bid goes, a trade prints at 98, a new bid at 97.9 absorbs the stop
    f.engine.cancel_order("AAPL", f.bid_id, 2);
    f.engine.submit_order(nrt::make_order(4, Side::BUY, OrderKind::LIMIT, "AAPL", 50, 98.0));
    f.engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 50, 98.0));
    f.engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 20000, 97.9));
    f.engine.try_release_waiting_orders(true);
    CHECK_EQ(f.holding(), base); // the stop sold the position
    f.bus.flush();
    f.bot->step();
    CHECK_EQ(f.bot->trades().size(), std::size_t{1});
    CHECK_EQ(f.bot->trades()[0].reason, std::string("SL"));
    CHECK(f.bot->trades()[0].r() < -0.9); // about one R lost
    CHECK_NEAR(f.bot->trades()[0].exit, 97.9, 1e-6); // the stop's real fill (the bid that absorbed it)
    CHECK_NEAR(f.bot->trades()[0].entry, 100.5, 1e-6); // the entry's real fill
    CHECK_EQ(f.open_orders_of_the_bot(), std::size_t{0}); // the target was cancelled (one-cancels-other)
    CHECK_EQ(f.bot->open_direction("AAPL"), 0);
}

TEST_CASE(rl_bot_holds_then_closes_when_the_agent_goes_flat) {
    auto fields = std::make_shared<std::string>(kRLLong);
    FakePolicyServer server(rl_server(fields));
    RLFixture f(server);
    Quantity base = f.holding();
    f.trade_day(0);
    f.trade_day(1); // the next day, same answer: hold
    CHECK_EQ(f.holding() - base, Quantity{500});
    CHECK_EQ(f.open_orders_of_the_bot(), std::size_t{2});
    CHECK(server.requests().back().find("dir=1 entry=100.5 sl=98.5 tp=104.5 days=0 tpr=2") != std::string::npos);
    *fields = kRLFlat;
    f.trade_day(2);
    CHECK_EQ(f.holding(), base); // sold at the open
    CHECK_EQ(f.open_orders_of_the_bot(), std::size_t{0});
    CHECK_EQ(f.bot->trades().back().reason, std::string("close"));
}

TEST_CASE(rl_bot_decides_in_the_pre_open_only) {
    auto fields = std::make_shared<std::string>(kRLLong);
    FakePolicyServer server(rl_server(fields));
    RLFixture f(server);
    f.session.set(0, SessionPhase::OPEN); // a day whose pre-open the bot never saw: no decision, counted as missed
    Quantity base = f.holding();
    f.bot->step();
    CHECK_EQ(f.holding(), base);
    CHECK_EQ(f.bot->decisions(), std::uint64_t{0});
    CHECK_EQ(f.bot->missed_opens(), std::uint64_t{1});
    f.session.set(1, SessionPhase::PRE_CLOSE); // nor later in the day
    f.bot->step();
    CHECK_EQ(f.bot->decisions(), std::uint64_t{0});
}


// ---------------------------------------------------------------- the policy client and the market feed
TEST_CASE(policy_client_reads_models_info_observes_and_acts) {
    auto fields = std::make_shared<std::string>(kRLLong);
    FakePolicyServer server(rl_server(fields));
    PolicyClient client;
    CHECK(client.connect("127.0.0.1", server.port()));
    CHECK_EQ(client.models().front(), std::string("bravo"));
    auto info = client.info("bravo");
    CHECK(info.has_value());
    CHECK_EQ(info->agent, std::string("ppo"));
    CHECK_EQ(info->lookback, 21);
    CHECK_EQ(info->train_seeds.size(), std::size_t{2});
    CHECK(!client.info("nope").has_value());

    ObservedBar bar; // one day's bar
    bar.close = 101.5;
    bar.bid = 101.0;
    bar.ask = 102.0;
    bar.bid_depth = 300;
    bar.ask_depth = 200;
    bar.volume = 7000;
    bar.open = 100.0;
    bar.high = 103.0;
    bar.low = 99.5;
    bar.trades = 150;
    CHECK(client.observe("AAPL", 12, bar));
    CHECK_EQ(server.requests().back(), std::string("OBSERVE AAPL 12 101.5 101 102 300 200 7000 100 103 99.5 150"));

    PolicyAction a = client.act("bravo", "AAPL", "dir=0 entry=0 sl=0 tp=0 days=0 tpr=0");
    CHECK(a.ok);
    CHECK_EQ(a.position, 1);
    CHECK_EQ(server.requests().back(), std::string("ACT bravo AAPL dir=0 entry=0 sl=0 tp=0 days=0 tpr=0"));
}

TEST_CASE(policy_answers_are_parsed_and_errors_kept_apart) {
    PolicyAction clamped = PolicyClient::parse_action("OK ACTION model=m symbol=S position=3 sl_atr=1 tp_r=2 atr=1");
    CHECK(clamped.ok && clamped.position == 1); // a direction, whatever the server says
    PolicyAction waiting = PolicyClient::parse_action("ERR NOT_ENOUGH_DATA have=12 need=21");
    CHECK(!waiting.ok && waiting.not_ready);
    PolicyAction error = PolicyClient::parse_action("ERR UNKNOWN_MODEL x");
    CHECK(!error.ok && !error.not_ready);
    PolicyAction garbage = PolicyClient::parse_action("OK ACTION model=m symbol=S");
    CHECK(!garbage.ok);
}

TEST_CASE(policy_info_lists_every_seed_an_agent_trained_on) {
    // an agent trained on several markets: the simulator must refuse it on ANY of their seeds
    FakePolicyServer server([](const std::string& request) {
        if (request == "INFO multi") return std::string("OK INFO model=multi agent=ppo lookback=21 train_seeds=301,302,310");
        return std::string("ERR UNKNOWN_COMMAND");
    });
    PolicyClient client;
    CHECK(client.connect("127.0.0.1", server.port()));
    auto info = client.info("multi");
    CHECK(info.has_value());
    CHECK_EQ(info->train_seeds.size(), std::size_t{3});
    CHECK(info->trained_on_seed(302) && info->trained_on_seed(310));
    CHECK(!info->trained_on_seed(42));
}

TEST_CASE(market_feed_pushes_every_symbols_daily_bar) {
    auto fields = std::make_shared<std::string>(kRLFlat);
    FakePolicyServer server(rl_server(fields));
    MarketFeed feed;
    CHECK(feed.connect("127.0.0.1", server.port()));
    DailyBar aapl;
    aapl.day = 3;
    aapl.symbol = "AAPL";
    aapl.open = 100.0;
    aapl.high = 104.0;
    aapl.low = 99.0;
    aapl.close = 103.0;
    aapl.volume = 5000;
    aapl.trades = 140;
    aapl.bid = 102.5;
    aapl.ask = 103.5;
    aapl.bid_depth = 400;
    aapl.ask_depth = 350;
    DailyBar msft = aapl;
    msft.symbol = "MSFT";
    std::size_t before = server.requests().size();
    feed.push_bars({aapl, msft});
    auto requests = server.requests();
    CHECK_EQ(requests.size() - before, std::size_t{2}); // one OBSERVE per symbol
    CHECK_EQ(requests[before], std::string("OBSERVE AAPL 3 103 102.5 103.5 400 350 5000 100 104 99 140"));
    CHECK_EQ(feed.days_sent(), std::uint64_t{1});
    CHECK_EQ(feed.bars_sent(), std::uint64_t{2});
}


// ---------------------------------------------------------------- a whole run with RL bots
TEST_CASE(a_run_with_rl_bots_reports_their_rl_lines_and_writes_rl_trades) {
    // an agent that reverses every day: long when flat, flat when long, so every second day closes a trade at the open
    FakePolicyServer server([](const std::string& request) {
        if (request == "MODELS") return std::string("OK MODELS bravo");
        if (request == "INFO bravo") return std::string("OK INFO model=bravo agent=ppo lookback=1 train_seeds=301,302");
        if (request.rfind("OBSERVE ", 0) == 0) return std::string("OK OBSERVED symbol=AAPL n=1");
        if (request.rfind("ACT bravo ", 0) == 0) {
            bool flat = request.find(" dir=0 ") != std::string::npos;
            return std::string("OK ACTION model=bravo symbol=AAPL ") + (flat ? "position=1 direction=1" : "position=0 direction=0") + " sl_atr=1 tp_r=2 atr=2 close=100";
        }
        return std::string("ERR UNKNOWN_COMMAND");
    });
    auto dir = nrt::fresh_output_dir("rl_run");
    SimulationConfig config;
    config.symbols = {{"AAPL", 100.0, 0.2}};
    config.noise_traders = 6;
    config.momentum_traders = 0;
    config.market_makers = 2;
    config.mean_reversion_traders = 0;
    config.trend_followers = 0;
    config.twap_executors = 0;
    config.rl_port = server.port();
    config.rl_bots_per_model = 1;
    config.bot_min_interval = std::chrono::milliseconds(10);
    config.bot_max_interval = std::chrono::milliseconds(30);
    config.session.days = 5;
    config.session.day = std::chrono::milliseconds(800);
    config.session.pre_open = std::chrono::milliseconds(150);
    config.session.pre_close = std::chrono::milliseconds(80);
    config.session.overnight = std::chrono::milliseconds(20);
    config.output_dir = dir.string();
    config.write_metrics = false;
    Simulation simulation(config);
    std::string report = simulation.run();
    CHECK(report.find("bravo rl: days=") != std::string::npos);
    CHECK(report.find("brackets") == std::string::npos);
    std::ifstream in(dir / "rl_trades_simu.csv");
    CHECK(in.is_open());
    std::string header;
    std::getline(in, header);
    CHECK_EQ(header, std::string("bot,symbol,direction,entry_day,exit_day,quantity,entry,exit,stop,target,sl_atr,tp_r,reason,r"));
    int rows = 0;
    for (std::string line; std::getline(in, line);) ++rows;
    CHECK(rows >= 1);
    CHECK(!std::filesystem::exists(dir / "bracket_trades_simu.csv"));
}

TEST_CASE(rl_bots_refuse_an_agent_trained_on_this_runs_seed) {
    FakePolicyServer server([](const std::string& request) {
        if (request == "MODELS") return std::string("OK MODELS leaky");
        if (request == "INFO leaky") return std::string("OK INFO model=leaky agent=ppo lookback=1 train_seeds=42,43");
        if (request.rfind("OBSERVE ", 0) == 0) return std::string("OK OBSERVED symbol=AAPL n=1");
        return std::string("ERR UNKNOWN_COMMAND");
    });
    SimulationConfig config;
    config.symbols = {{"AAPL", 100.0, 0.2}};
    config.noise_traders = 2;
    config.momentum_traders = 0;
    config.market_makers = 1;
    config.mean_reversion_traders = 0;
    config.trend_followers = 0;
    config.twap_executors = 0;
    config.seed = 42;
    config.rl_port = server.port();
    config.session.days = 1;
    config.session.day = std::chrono::milliseconds(200);
    config.session.overnight = std::chrono::milliseconds(10);
    Simulation simulation(config);
    std::string report = simulation.run();
    CHECK(report.find("leaky rl:") == std::string::npos);
    CHECK(report.find("1 RL agent(s) skipped: trained on this very simulation seed") != std::string::npos);
}

