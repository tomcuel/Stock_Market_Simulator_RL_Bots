// Tests for the end-of-run market report (MarketRecorder) and NotificationBus::flush(), which the report relies on to include every trade that was already executed
#include <atomic>
#include <filesystem>
#include <random>
#include <thread>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "market_recorder.hpp"
#include "matching_engine.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"

using namespace sim;
namespace fs = std::filesystem;

namespace {

fs::path fresh_temp_dir(const std::string& tag) {
    return nrt::fresh_output_dir("report_" + tag); // NRT/output/report_<tag>_N
}

std::vector<std::string> read_lines(const fs::path& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

// value column of summary_simu.csv for `key`, or "" if absent
std::string summary_value(const fs::path& dir, const std::string& key) {
    for (const auto& line : read_lines(dir / "summary_simu.csv")) {
        if (line.rfind(key + ",", 0) == 0) return line.substr(key.size() + 1);
    }
    return "";
}

} // namespace

TEST_CASE(notification_bus_flush_waits_until_every_event_is_delivered) {
    std::atomic<int> delivered{0};
    NotificationBus bus; // declared after `delivered`, so destroyed first
    bus.subscribe([&](const Event&) {
        std::this_thread::sleep_for(std::chrono::microseconds(20)); // slow subscriber, so events pile up
        delivered.fetch_add(1);
    });
    for (int i = 0; i < 500; ++i) {
        bus.publish(OrderQueuedEvent{static_cast<OrderId>(i), 1, "AAPL"});
    }
    bus.flush();
    CHECK_EQ(delivered.load(), 500); // not "eventually": exactly all of them, at the moment flush() returns
}

TEST_CASE(resting_and_waiting_order_accessors_list_individual_orders) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);
    engine.grant_initial_holdings(1, "AAPL", 50);

    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 3, 95.0));
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 4, 105.0));
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 2, 80.0)); // waits

    auto resting = engine.resting_orders("AAPL");
    CHECK_EQ(resting.size(), std::size_t{2});
    CHECK_EQ(engine.waiting_orders().size(), std::size_t{1});
    CHECK(engine.waiting_orders().front().kind == OrderKind::STOP);
    CHECK(engine.resting_orders("NOT_A_SYMBOL").empty()); // unknown symbol: empty, never throws
}

TEST_CASE(market_report_contains_trades_resting_and_waiting_orders_and_before_after_prices) {
    fs::path dir = fresh_temp_dir("content");
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.register_symbol("MSFT", 300.0);
    engine.ensure_client(1, 100000.0);
    engine.ensure_client(2, 100000.0);
    engine.grant_initial_holdings(1, "AAPL", 50);

    RecorderConfig config;
    config.output_dir = dir.string();
    config.file_suffix = "_simu";
    config.sample_interval = std::chrono::milliseconds(20);
    MarketRecorder recorder(bus, engine, config);
    recorder.start();

    // one completed trade at 102, one order left resting, one STOP left waiting
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 102.0));
    engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 5, 102.0));
    engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 2, 99.0));
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 3, 90.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(60)); // let a few samples happen

    recorder.stop();
    CHECK(recorder.write_report());

    // every file carries the configured suffix, and nothing is written without it
    for (const char* base : {"summary", "symbols", "trades", "price_samples", "portfolio_samples",
                             "portfolios_final", "order_book_final", "resting_orders", "waiting_orders", "rejections"}) {
        CHECK(fs::exists(dir / (std::string(base) + "_simu.csv")));
        CHECK(!fs::exists(dir / (std::string(base) + ".csv")));
    }

    CHECK_EQ(read_lines(dir / "trades_simu.csv").size(), std::size_t{2});          // header + 1 trade
    CHECK_EQ(read_lines(dir / "resting_orders_simu.csv").size(), std::size_t{2});  // header + the bid at 99
    CHECK_EQ(read_lines(dir / "waiting_orders_simu.csv").size(), std::size_t{2});  // header + the STOP
    CHECK_EQ(summary_value(dir, "trades"), std::string("1"));
    CHECK_EQ(summary_value(dir, "resting_orders_final"), std::string("1"));
    CHECK_EQ(summary_value(dir, "waiting_orders_final"), std::string("1"));
    CHECK(read_lines(dir / "price_samples_simu.csv").size() > 3); // several samples x 2 symbols

    // before vs after: AAPL started at 100, last traded at 102; MSFT never traded
    bool saw_aapl = false, saw_msft = false;
    for (const auto& line : read_lines(dir / "symbols_simu.csv")) {
        if (line.rfind("AAPL,100,102,2,", 0) == 0) saw_aapl = true;   // symbol,initial,final,change_pct(=2%)...
        if (line.rfind("MSFT,300,300,0,", 0) == 0) saw_msft = true;
    }
    CHECK(saw_aapl);
    CHECK(saw_msft);

    fs::remove_all(dir);
}

TEST_CASE(market_report_conserves_cash_across_many_concurrent_trades) {
    // total cash is only ever moved between clients by a trade, never created or destroyed: the report's before/after totals must agree even with many bots trading concurrently
    fs::path dir = fresh_temp_dir("cash");
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    const int num_clients = 8;
    for (ClientId id = 1; id <= num_clients; ++id) {
        engine.ensure_client(id, 50000.0);
        engine.grant_initial_holdings(id, "AAPL", 100);
    }
    RecorderConfig config;
    config.output_dir = dir.string();
    config.file_suffix = "_simu";
    config.sample_interval = std::chrono::milliseconds(10);
    MarketRecorder recorder(bus, engine, config);
    recorder.start();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_clients; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t + 1);
            std::uniform_real_distribution<double> price_dist(95.0, 105.0);
            std::bernoulli_distribution side_dist(0.5);
            for (int i = 0; i < 150; ++i) {
                Side side = side_dist(rng) ? Side::BUY : Side::SELL;
                engine.submit_order(nrt::make_order(t + 1, side, OrderKind::LIMIT, "AAPL", 1, price_dist(rng)));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    recorder.stop();
    CHECK(recorder.write_report());

    double before = std::stod(summary_value(dir, "total_cash_initial"));
    double after = std::stod(summary_value(dir, "total_cash_final"));
    CHECK_NEAR(before, 50000.0 * num_clients, 1e-6);
    CHECK_NEAR(after, before, 1e-6);
    // every trade executed was captured: the report's count matches what the recorder saw live
    CHECK_EQ(summary_value(dir, "trades"), std::to_string(recorder.recorded_trade_count()));
    CHECK(recorder.recorded_trade_count() > 0);

    fs::remove_all(dir);
}

TEST_CASE(market_report_to_an_unwritable_directory_fails_cleanly) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    // a regular FILE where a directory component should be: create_directories() must fail, on every OS (unlike a hardcoded system path, which is only unwritable on some platforms)
    fs::path blocker = nrt::fresh_output_dir("report_unwritable") / "i_am_a_file";
    { std::ofstream(blocker) << "not a directory"; }
    RecorderConfig config;
    config.output_dir = (blocker / "report").string();
    MarketRecorder recorder(bus, engine, config);
    recorder.start();
    recorder.stop();
    CHECK(!recorder.write_report()); // reports failure instead of crashing or throwing
}

TEST_CASE(recorder_destroyed_before_the_bus_while_events_are_still_flowing_is_safe) {
    // the use-after-free pattern from notification.hpp, applied to the recorder: it must unsubscribe in its destructor so the bus's worker thread can never call into a destroyed recorder
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000000.0);
    engine.ensure_client(2, 1000000.0);
    engine.grant_initial_holdings(1, "AAPL", 10000);
    {
        RecorderConfig config;
        config.output_dir = fresh_temp_dir("lifetime").string(); // never written: no write_report() call
        MarketRecorder recorder(bus, engine, config);
        recorder.start();
        for (int i = 0; i < 300; ++i) {
            engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 1, 100.0));
            engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 1, 100.0));
        }
        // recorder goes out of scope here with trade events very likely still queued on the bus
    }
    bus.flush(); // the remaining events are delivered to nobody and nothing crashes
    CHECK(true);
}
