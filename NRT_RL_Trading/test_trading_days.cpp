//=======================================================================
// Tests for the trading days (simulation.x --days, market_session.hpp) and the fundamental values (fundamentals.hpp):
// - MARKET orders in an auction join at their collar and expire after it
// - the market reopens after a close, and a GTC order's clock skips the closed hours
// - overnight news moves the reference price the next day opens from
// - the daily bar follows the day's trades and the closing price
// - a short run with trading days writes its daily bars and reports them
// - the fundamental values' variance split between the night and the day, and value investors buying a cheap stock
//=======================================================================
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "bot.hpp"
#include "fundamentals.hpp"
#include "gateway.hpp"
#include "market_session.hpp"
#include "matching_engine.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "simulation.hpp"
#include "test_helpers.hpp"

using namespace sim;

namespace {

void market(MatchingEngine& engine) {
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000000.0);
    engine.ensure_client(2, 1000000.0);
    engine.grant_initial_holdings(1, "AAPL", 1000);
    engine.grant_initial_holdings(2, "AAPL", 1000);
}

} // namespace

TEST_CASE(market_orders_join_an_auction_at_their_collar_and_expire_after_it) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.set_auction_mode(true);
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 101.0));
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::MARKET, "AAPL", 5));     // market-on-open: joins at 102, first in line
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::MARKET, "AAPL", 4));    // joins at 98
    auto opens = engine.run_opening_auction();
    CHECK(opens[0].crossed);
    CHECK_EQ(opens[0].volume, Quantity{4});
    CHECK(opens[0].price >= 98.0 && opens[0].price <= 102.0);
    CHECK_EQ(engine.expire_market_orders(), std::size_t{1}); // the market buy's last share: immediate or cancel, never carried
    for (const auto& open : engine.open_orders()) {
        CHECK(open.order.kind != OrderKind::MARKET);
    }
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 10 * 101.0, 1e-6); // only the limit buy still reserves cash: nothing leaked
}

TEST_CASE(the_market_reopens_and_a_gtc_clock_skips_the_closed_hours) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    auto gtc = nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 90.0);
    gtc.expires_in = std::chrono::seconds(60);
    engine.submit_order(gtc);
    auto expiry_before = *engine.open_orders().front().order.expires_at;
    engine.close_market();
    CHECK(!engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 1, 95.0)).accepted); // closed
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    engine.open_market();
    CHECK(!engine.market_closed());
    CHECK(engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 1, 95.0)).accepted);
    auto expiry_after = *engine.open_orders().front().order.expires_at;
    CHECK(expiry_after - expiry_before >= std::chrono::milliseconds(35)); // the night didn't count
}

TEST_CASE(overnight_news_moves_the_price_the_next_day_opens_from) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    CHECK_NEAR(engine.gap_reference_price("AAPL", std::exp(0.02)), 100.0 * std::exp(0.02), 1e-9);
    CHECK_NEAR(engine.last_price("AAPL"), 100.0 * std::exp(0.02), 1e-9);
}

TEST_CASE(the_daily_bar_follows_the_days_trades_and_the_close) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    DailyBarBuilder bars(bus);
    bars.start_day(3, 0.0, engine.all_last_prices(), {{"AAPL", 0.01}});
    for (Price p : {101.0, 104.0, 99.0}) {
        engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, p));
        engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::MARKET, "AAPL", 10));
    }
    bars.set_book("AAPL", 98.5, 99.5, 100.0, 50.0);
    bus.flush();
    auto day = bars.finish_day(1000.0, {{"AAPL", 100.0}}, {{"AAPL", 5}});
    CHECK_EQ(day.size(), std::size_t{1});
    const DailyBar& bar = day[0];
    CHECK_EQ(bar.day, 3);
    CHECK_NEAR(bar.open, 101.0, 1e-9);
    CHECK_NEAR(bar.high, 104.0, 1e-9);
    CHECK_NEAR(bar.low, 99.0, 1e-9);
    CHECK_NEAR(bar.close, 100.0, 1e-9); // the official close, not the last trade
    CHECK_EQ(bar.volume, Quantity{30});
    CHECK_EQ(bar.trades, 3);
    CHECK_NEAR(bar.vwap, (101.0 + 104.0 + 99.0) / 3.0, 1e-9);
    CHECK_NEAR(bar.gap, 0.01, 1e-12);
    CHECK_EQ(bar.auction_volume, Quantity{5});
    CHECK_NEAR(bar.bid, 98.5, 1e-12);
}

TEST_CASE(the_session_only_accepts_orders_while_the_exchange_is_open) {
    MarketSession session;
    CHECK(!session.accepting_orders()); // before the first open
    session.set(0, SessionPhase::PRE_OPEN);
    CHECK(session.accepting_orders() && !session.continuous_trading());
    session.set(0, SessionPhase::OPEN);
    CHECK(session.continuous_trading());
    session.set(0, SessionPhase::CLOSED);
    CHECK(!session.accepting_orders());
    CHECK_EQ(session.day(), 0);
}

TEST_CASE(a_run_with_trading_days_writes_and_reports_its_daily_bars) {
    auto dir = nrt::fresh_output_dir("trading_days");
    SimulationConfig config;
    config.symbols = {{"AAPL", 100.0, 0.2}, {"MSFT", 200.0, 0.2}};
    config.noise_traders = 4;
    config.momentum_traders = 0;
    config.market_makers = 2;
    config.mean_reversion_traders = 0;
    config.trend_followers = 0;
    config.twap_executors = 0;
    config.value_investors = 4;
    config.session.days = 3;
    config.session.day = std::chrono::milliseconds(400);
    config.session.pre_open = std::chrono::milliseconds(40);
    config.session.pre_close = std::chrono::milliseconds(40);
    config.session.overnight = std::chrono::milliseconds(20);
    config.output_dir = dir.string();
    config.write_metrics = false;
    Simulation simulation(config);
    std::string report = simulation.run();
    CHECK(report.find("-- Trading days (3 days") != std::string::npos);
    // the value investors see the hidden value: reported apart, after the ranked strategies, never among them
    auto ranked = report.find("-- Strategies (they only see prices");
    auto informed = report.find("-- Informed benchmark (sees the hidden fundamental value");
    CHECK(ranked != std::string::npos && informed != std::string::npos && ranked < informed);
    CHECK(report.find("\nvalue: bots=4", informed) != std::string::npos);
    CHECK(report.substr(ranked, informed - ranked).find("\nvalue:") == std::string::npos);
    CHECK(report.substr(ranked, informed - ranked).find("\nnoise: bots=4") != std::string::npos);
    CHECK(is_informed_benchmark("value") && !is_informed_benchmark("noise") && !is_informed_benchmark("rl"));
    std::ifstream in(dir / "daily_bars_simu.csv");
    CHECK(in.is_open());
    std::string line;
    int rows = -1; // the header
    while (std::getline(in, line)) ++rows;
    CHECK_EQ(rows, 6); // 3 days x 2 symbols
    // every phase of every day, in order, on the samples' clock
    std::ifstream phases(dir / "session_phases_simu.csv");
    std::vector<std::string> lines;
    for (std::string l; std::getline(phases, l);) lines.push_back(l);
    CHECK_EQ(lines.size(), std::size_t{13}); // header + 3 days x 4 phases
    const char* order[] = {"PRE_OPEN", "OPEN", "PRE_CLOSE", "CLOSED"};
    double last = -1.0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::string expected = std::to_string((i - 1) / 4) + "," + order[(i - 1) % 4] + ",";
        CHECK(lines[i].rfind(expected, 0) == 0);
        double t = std::stod(lines[i].substr(lines[i].rfind(',') + 1));
        CHECK(t >= last);
        last = t;
    }
}

TEST_CASE(trading_days_are_the_only_mode_a_zero_day_run_still_trades_one_day) {
    // there is no continuous mode any more: a config asking for 0 days runs one whole trading day
    SimulationConfig config;
    config.symbols = {{"AAPL", 100.0, 0.2}};
    config.noise_traders = 2;
    config.momentum_traders = 0;
    config.market_makers = 1;
    config.mean_reversion_traders = 0;
    config.trend_followers = 0;
    config.twap_executors = 0;
    config.session.days = 0;
    config.session.day = std::chrono::milliseconds(200);
    config.session.overnight = std::chrono::milliseconds(10);
    Simulation simulation(config);
    std::string report = simulation.run();
    CHECK(report.find("-- Trading days (1 days") != std::string::npos);
}

TEST_CASE(fundamental_values_split_their_variance_between_the_night_and_the_day) {
    FundamentalValues values(7, 0.35, 0.0);
    const double vol = 0.02;
    double night = 0.0, day = 0.0;
    const int n = 4000;
    for (int i = 0; i < n; ++i) {
        values.add("X", 100.0, vol);
        night += std::pow(values.overnight()["X"], 2);
        values.add("X", 100.0, vol);
        values.diffuse(1.0);
        double r = std::log(values.value("X") / 100.0);
        day += r * r;
    }
    CHECK_NEAR(std::sqrt(night / n), vol * std::sqrt(0.35), 0.1 * vol * std::sqrt(0.35));
    CHECK_NEAR(std::sqrt(day / n), vol * std::sqrt(0.65), 0.1 * vol * std::sqrt(0.65));
}

TEST_CASE(a_value_investor_buys_a_stock_trading_below_its_value) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    for (ClientId id = 1; id <= 3; ++id) engine.fund_balanced_account(id, 10000000.0, 0.5);
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 50000, 100.5));
    engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 50000, 99.5));
    FundamentalValues values(1, 0.35, 0.0);
    values.add("AAPL", 120.0, 0.0126); // worth 20% more than it trades at: far beyond 1.5 days of volatility
    InProcessGateway gateway(engine, 3);
    ValueInvestorBot bot(gateway, {"AAPL"}, 5, {}, &values);
    Quantity before = engine.portfolio_snapshot(3).holding("AAPL");
    for (int i = 0; i < 5; ++i) bot.step();
    CHECK(engine.portfolio_snapshot(3).holding("AAPL") > before);
}
