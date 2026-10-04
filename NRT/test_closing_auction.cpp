//=======================================================================
// Tests for the end of the trading day: 
// - pre-close (orders rest without matching)
// - the closing auction (one price per symbol, the one executing the most volume)
// - day orders expiring with their reservations released
// - GTC / dated orders carried to the next session through the snapshot, which then opens with an auction
//=======================================================================
#include <thread>

#include "matching_engine.hpp"
#include "net/auth.hpp"
#include "net/persistence.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"

using namespace sim;

namespace {

// clients 1 (buyer) and 2 (seller) with plenty of cash and shares of AAPL, last price 100
void market(MatchingEngine& engine) {
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000000.0);
    engine.ensure_client(2, 1000000.0);
    engine.grant_initial_holdings(1, "AAPL", 1000);
    engine.grant_initial_holdings(2, "AAPL", 1000);
}

} // namespace

TEST_CASE(pre_close_orders_rest_without_matching) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.set_auction_mode(true);
    auto buy = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 105.0));
    auto sell = engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 95.0));
    CHECK(buy.accepted && sell.accepted);
    CHECK_EQ(buy.filled_quantity + sell.filled_quantity, Quantity{0}); // they cross, yet nothing traded
    auto snap = engine.snapshot("AAPL", 5);
    CHECK(snap.bids.front().price > snap.asks.front().price);         // a crossed book, until the fixing
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 1050.0, 1e-9); // reserved as usual
}

TEST_CASE(closing_auction_picks_the_price_executing_the_most_volume) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.set_auction_mode(true);
    for (Price p : {102.0, 101.0, 100.0}) engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, p));
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 99.0));
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 100.0));
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 15, 101.0));
    double cash_before = engine.portfolio_snapshot(1).cash + engine.portfolio_snapshot(2).cash;

    auto closes = engine.run_closing_auction();
    CHECK_EQ(closes.size(), std::size_t{1});
    // 20 shares can trade at 100 (demand 30, supply 20) and at 101 (demand 20, supply 35): the smaller imbalance wins, 100
    CHECK(closes[0].crossed);
    CHECK_NEAR(closes[0].price, 100.0, 1e-12);
    CHECK_EQ(closes[0].volume, Quantity{20});
    CHECK_NEAR(engine.last_price("AAPL"), 100.0, 1e-12); // the official close
    CHECK(!engine.auction_mode());                       // pre-close is over
    CHECK_EQ(engine.portfolio_snapshot(1).holding("AAPL"), Quantity{1020});
    CHECK_NEAR(engine.portfolio_snapshot(1).cash, 1000000.0 - 20 * 100.0, 1e-9); // everyone paid the single closing price
    CHECK_NEAR(engine.portfolio_snapshot(1).cash + engine.portfolio_snapshot(2).cash, cash_before, 1e-9); // only moved
    // the buyer's two filled orders (102, 101) released their whole reservation, only the unfilled 100 bid remains reserved
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 10 * 100.0, 1e-9);
}

TEST_CASE(closing_auction_without_a_cross_keeps_the_last_price) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.set_auction_mode(true);
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 98.0));
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 102.0));
    auto closes = engine.run_closing_auction();
    CHECK(!closes[0].crossed);
    CHECK_EQ(closes[0].volume, Quantity{0});
    CHECK_NEAR(closes[0].price, 100.0, 1e-12);
}

TEST_CASE(waiting_orders_are_not_released_during_pre_close) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    auto stop = engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::STOP, "AAPL", 5, 99.0)); // waits for 99
    CHECK(stop.queued);
    engine.set_auction_mode(true);
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 5, 98.0));
    auto summary = engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK(summary.released.empty()); // even if its band were reached, it would wait
    CHECK_EQ(engine.waiting_orders().size(), std::size_t{1});
}

TEST_CASE(day_orders_expire_and_release_every_reservation) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 90.0));    // resting
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 110.0));  // resting
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::STOP, "AAPL", 20, 95.0));    // waiting
    CHECK(engine.portfolio_snapshot(1).reserved_cash > 0.0);
    CHECK_EQ(engine.portfolio_snapshot(2).reserved("AAPL"), Quantity{30});

    CHECK_EQ(engine.expire_all_orders(), std::size_t{3});
    CHECK(engine.resting_orders("AAPL").empty());
    CHECK(engine.waiting_orders().empty());
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 0.0, 1e-9); // the next session starts with nothing engaged
    CHECK_EQ(engine.portfolio_snapshot(2).reserved("AAPL"), Quantity{0});
    CHECK_EQ(engine.portfolio_snapshot(2).holding("AAPL"), Quantity{1000}); // expiring is not selling
}

namespace {

OrderRequest with_tif(OrderRequest request, TimeInForce tif) {
    request.time_in_force = tif;
    return request;
}

std::string snapshot_path(const std::string& tag) {
    return (nrt::fresh_output_dir(tag) / "day.snapshot").string();
}

} // namespace

TEST_CASE(only_day_orders_expire_at_the_close) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 90.0));                     // DAY (default)
    auto gtc = engine.submit_order(with_tif(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 91.0), TimeInForce::GTC));
    OrderRequest dated = nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 92.0);
    dated.expires_in = std::chrono::seconds(3600);                                                                // GTC until its date
    auto gtd = engine.submit_order(dated);

    CHECK_EQ(engine.expire_day_orders(), std::size_t{1});
    auto open = engine.open_orders();
    CHECK_EQ(open.size(), std::size_t{2});
    CHECK((open[0].order.id == gtc.order_id || open[0].order.id == gtd.order_id));
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 10 * 91.0 + 10 * 92.0, 1e-9); // the survivors stay reserved
}

TEST_CASE(gtc_orders_survive_the_session_through_the_snapshot) {
    std::string path = snapshot_path("gtc_snapshot");
    std::vector<MatchingEngine::OpenOrder> before;
    double reserved_cash = 0.0;
    Quantity reserved_shares = 0;
    OrderId last_id = 0;
    {
        NotificationBus bus;
        MatchingEngine engine(bus);
        net::ClientDirectory directory;
        market(engine);
        engine.submit_order(with_tif(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 95.0), TimeInForce::GTC));
        engine.submit_order(with_tif(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 7, 95.0), TimeInForce::GTC)); // same level, later
        engine.submit_order(with_tif(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 105.0), TimeInForce::GTC));
        last_id = engine.submit_order(with_tif(nrt::make_order(2, Side::SELL, OrderKind::STOP, "AAPL", 20, 90.0), TimeInForce::GTC)).order_id;
        engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 3, 80.0)); // DAY
        engine.expire_day_orders();                                                            // the close
        before = engine.open_orders();
        reserved_cash = engine.portfolio_snapshot(1).reserved_cash;
        reserved_shares = engine.portfolio_snapshot(2).reserved("AAPL");
        CHECK(net::PersistenceStore::save(path, directory, engine));
    }
    // the next session: a new engine, reloaded from the snapshot
    NotificationBus bus;
    MatchingEngine engine(bus);
    net::ClientDirectory directory;
    CHECK(net::PersistenceStore::load(path, directory, engine));
    auto after = engine.open_orders();
    CHECK_EQ(after.size(), before.size());
    CHECK_EQ(after.size(), std::size_t{4});
    for (std::size_t i = 0; i < std::min(after.size(), before.size()); ++i) {
        CHECK_EQ(after[i].order.id, before[i].order.id); // same ids, same order: the time priority at 95 is kept
        CHECK_EQ(after[i].order.quantity, before[i].order.quantity);
        CHECK_NEAR(after[i].order.price, before[i].order.price, 1e-12);
        CHECK(after[i].waiting == before[i].waiting);
        CHECK(after[i].order.time_in_force == TimeInForce::GTC);
    }
    // reservations are not saved: they are rebuilt from the restored orders, to the same amounts
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, reserved_cash, 1e-9);
    CHECK_EQ(engine.portfolio_snapshot(2).reserved("AAPL"), reserved_shares);
    // new orders never reuse a restored id
    auto next = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 1, 50.0));
    CHECK(next.order_id > last_id);
}

TEST_CASE(carried_over_orders_that_cross_open_with_an_auction) {
    std::string path = snapshot_path("opening_auction");
    {
        NotificationBus bus;
        MatchingEngine engine(bus);
        net::ClientDirectory directory;
        market(engine);
        engine.set_auction_mode(true); // collected without matching, then saved before any auction
        engine.submit_order(with_tif(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 104.0), TimeInForce::GTC));
        engine.submit_order(with_tif(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 6, 98.0), TimeInForce::GTC));
        CHECK(net::PersistenceStore::save(path, directory, engine));
    }
    NotificationBus bus;
    MatchingEngine engine(bus);
    net::ClientDirectory directory;
    CHECK(net::PersistenceStore::load(path, directory, engine));
    auto opens = engine.run_opening_auction();
    CHECK_EQ(opens.size(), std::size_t{1});
    CHECK(opens[0].crossed);
    CHECK_EQ(opens[0].volume, Quantity{6});
    CHECK(opens[0].price >= 98.0 && opens[0].price <= 104.0);
    CHECK_EQ(engine.portfolio_snapshot(1).holding("AAPL"), Quantity{1006});
    CHECK_EQ(engine.open_orders().size(), std::size_t{1}); // the 4 unfilled shares of the bid wait for the session
}

TEST_CASE(a_dated_order_keeps_its_remaining_time_across_sessions) {
    std::string path = snapshot_path("dated_order");
    {
        NotificationBus bus;
        MatchingEngine engine(bus);
        net::ClientDirectory directory;
        market(engine);
        OrderRequest dated = nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 90.0);
        dated.expires_in = std::chrono::seconds(60);
        engine.submit_order(dated);
        CHECK(net::PersistenceStore::save(path, directory, engine));
    }
    NotificationBus bus;
    MatchingEngine engine(bus);
    net::ClientDirectory directory;
    CHECK(net::PersistenceStore::load(path, directory, engine));
    auto open = engine.open_orders();
    CHECK_EQ(open.size(), std::size_t{1});
    CHECK(open[0].order.expires_at.has_value());
    // trading time: the time it had left when saved, counted from this session's opening (the market was closed between)
    auto left = std::chrono::duration_cast<std::chrono::seconds>(*open[0].order.expires_at - Clock::now()).count();
    CHECK(left >= 58 && left <= 60);
}

TEST_CASE(a_closed_market_rejects_new_orders) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.close_market();
    auto order = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 100.0));
    CHECK(!order.accepted);
    CHECK_EQ(order.reject_reason, std::string("market closed"));
    CHECK(engine.resting_orders("AAPL").empty());
}

TEST_CASE(market_orders_and_expired_orders_are_never_carried_over) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine);
    engine.set_auction_mode(true);
    OrderRequest market_buy = nrt::make_order(1, Side::BUY, OrderKind::MARKET, "AAPL", 10);
    market_buy.expires_in = std::chrono::seconds(3600); // an expiry makes it GTC, yet a MARKET order is immediate or cancel
    engine.submit_order(market_buy);
    OrderRequest past = nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 90.0);
    past.expires_in = std::chrono::seconds(0); // its date is already reached at the close
    engine.submit_order(past);
    engine.submit_order(with_tif(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 91.0), TimeInForce::GTC));
    engine.close_market();
    engine.run_closing_auction(); // nothing to cross with
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(engine.expire_day_orders(), std::size_t{2});
    auto open = engine.open_orders();
    CHECK_EQ(open.size(), std::size_t{1}); // only the GTC limit order lives on
    CHECK(open[0].order.kind == OrderKind::LIMIT && open[0].order.time_in_force == TimeInForce::GTC);
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 10 * 91.0, 1e-9);
}

TEST_CASE(an_order_clock_stops_at_the_close) {
    std::string path = snapshot_path("clock_stops");
    {
        NotificationBus bus;
        MatchingEngine engine(bus);
        net::ClientDirectory directory;
        market(engine);
        OrderRequest dated = nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 90.0);
        dated.expires_in = std::chrono::seconds(2);
        engine.submit_order(dated);
        engine.close_market();
        engine.expire_day_orders();                                   // 2 s left at the close: carried over
        std::this_thread::sleep_for(std::chrono::milliseconds(2300)); // the server takes its time to stop
        CHECK(net::PersistenceStore::save(path, directory, engine));  // its date is now behind, in wall time
    }
    NotificationBus bus;
    MatchingEngine engine(bus);
    net::ClientDirectory directory;
    CHECK(net::PersistenceStore::load(path, directory, engine));
    CHECK_EQ(engine.open_orders().size(), std::size_t{1}); // but in trading time it still had its 2 s: restored
}
