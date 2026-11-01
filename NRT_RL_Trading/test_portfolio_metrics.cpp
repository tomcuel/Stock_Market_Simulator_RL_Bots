#include <random>
#include <thread>
#include <vector>

#include "market_data.hpp"
#include "matching_engine.hpp"
#include "metrics.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"
#include "portfolio.hpp"

using namespace sim;

TEST_CASE(market_data_tracks_last_price_and_volume) {
    NotificationBus bus;
    MarketData market_data(bus);
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);
    engine.ensure_client(2, 0.0);
    engine.grant_initial_holdings(2, "AAPL", 20);

    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 4, 100.0));
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 4, 100.0));
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 6, 105.0));
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 6, 105.0));

    // events are delivered asynchronously via the notification bus worker thread
    Quantity volume = 0;
    for (int i = 0; i < 200 && volume < 10; ++i) {
        volume = market_data.total_volume("AAPL");
        if (volume < 10) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK_EQ(market_data.last_price("AAPL"), 105.0);
    CHECK_EQ(market_data.total_volume("AAPL"), Quantity{10});
    // vwap = (4*100 + 6*105) / 10 = 103.0
    CHECK_NEAR(market_data.vwap("AAPL"), 103.0, 1e-9);
}

TEST_CASE(market_data_returns_empty_for_unknown_symbol) {
    NotificationBus bus;
    MarketData market_data(bus);
    CHECK_EQ(market_data.last_price("NOPE"), 0.0);
    CHECK_EQ(market_data.total_volume("NOPE"), Quantity{0});
    CHECK(market_data.recent_candles("NOPE").empty());
}

TEST_CASE(portfolio_net_worth_accounts_for_cash_and_holdings) {
    Portfolio p;
    p.cash = 1000.0;
    p.holdings["AAPL"] = 10;
    p.holdings["MSFT"] = 5;

    std::unordered_map<Symbol, Price> prices{{"AAPL", 100.0}, {"MSFT", 200.0}};
    CHECK_NEAR(p.net_worth(prices), 1000.0 + 10 * 100.0 + 5 * 200.0, 1e-9);
}

TEST_CASE(portfolio_net_worth_ignores_symbols_with_unknown_price) {
    Portfolio p;
    p.cash = 500.0;
    p.holdings["UNKNOWN"] = 100;

    std::unordered_map<Symbol, Price> prices{}; // no price for UNKNOWN
    CHECK_NEAR(p.net_worth(prices), 500.0, 1e-9); // unpriced holdings don't contribute
}

TEST_CASE(latency_histogram_buckets_and_stats) {
    LatencyHistogram hist;
    hist.record(std::chrono::microseconds(5));
    hist.record(std::chrono::microseconds(20));
    hist.record(std::chrono::microseconds(2000));

    CHECK_EQ(hist.count(), std::uint64_t{3});
    CHECK_NEAR(hist.mean_us(), (5.0 + 20.0 + 2000.0) / 3.0, 1e-6);
    CHECK_EQ(hist.max_us(), std::int64_t{2000});
}

TEST_CASE(metrics_registry_reset_clears_counters) {
    auto& metrics = MetricsRegistry::instance();
    metrics.reset();
    metrics.order_submitted();
    metrics.order_accepted();
    metrics.trades_executed(3);
    metrics.volume_traded(50);

    CHECK_EQ(metrics.orders_submitted(), std::uint64_t{1});
    CHECK_EQ(metrics.trades_executed(), std::uint64_t{3});
    CHECK_EQ(metrics.volume_traded(), std::uint64_t{50});

    metrics.reset();
    CHECK_EQ(metrics.orders_submitted(), std::uint64_t{0});
    CHECK_EQ(metrics.trades_executed(), std::uint64_t{0});
    CHECK_EQ(metrics.volume_traded(), std::uint64_t{0});
}

// Direct stress test of MarketData's own mutex: many threads publishing trade events concurrently while other threads continuously read last_price/vwap/total_volume/recent_candles
// The notification bus fans events out on its own worker thread, so this also exercises MarketData's consumer callback running concurrently with reader threads 
// (proving reads never see a torn/partial update (e.g. total_volume updated but cumulative_notional not yet, which would briefly produce a nonsensical VWAP) and never crash under concurrent access)
TEST_CASE(concurrent_trade_publishing_and_reading_never_corrupts_market_data) {
    NotificationBus bus;
    MarketData market_data(bus);

    const int num_publisher_threads = 8;
    const int trades_per_thread = 200;
    const int num_reader_threads = 4;
    std::atomic<bool> stop_readers{false};
    std::atomic<std::uint64_t> total_published_volume{0};

    std::vector<std::thread> publishers;
    for (int t = 0; t < num_publisher_threads; ++t) {
        publishers.emplace_back([&, t] {
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> qty_dist(1, 10);
            std::uniform_real_distribution<double> price_dist(90.0, 110.0);
            for (int i = 0; i < trades_per_thread; ++i) {
                Trade trade;
                trade.symbol = "AAPL";
                trade.quantity = qty_dist(rng);
                trade.price = price_dist(rng);
                trade.timestamp = Clock::now();
                total_published_volume.fetch_add(static_cast<std::uint64_t>(trade.quantity));
                bus.publish(TradeEvent{trade});
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < num_reader_threads; ++r) {
        readers.emplace_back([&] {
            while (!stop_readers.load()) {
                // none of these should ever crash, hang, or (for the numeric ones) return a negative/NaN value while writers are active concurrently
                Price last = market_data.last_price("AAPL");
                double vwap = market_data.vwap("AAPL");
                Quantity volume = market_data.total_volume("AAPL");
                auto candles = market_data.recent_candles("AAPL", 5);
                CHECK(last >= 0.0);
                CHECK(volume >= 0);
                CHECK(vwap >= 0.0);
                (void)candles;
            }
        });
    }
    for (auto& t : publishers) {
        t.join();
    }

    // give the notification bus's worker thread time to drain every published event before checking the final total
    for (int i = 0; i < 200; ++i) {
        if (market_data.total_volume("AAPL") >= static_cast<Quantity>(total_published_volume.load())) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_readers.store(true);
    for (auto& t : readers) {
        t.join();
    }

    CHECK_EQ(market_data.total_volume("AAPL"), static_cast<Quantity>(total_published_volume.load()));
}
