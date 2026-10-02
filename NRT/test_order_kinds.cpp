#include "matching_engine.hpp"
#include "nrt_framework.hpp"
#include "notification.hpp"
#include "order_registry.hpp"
#include "test_helpers.hpp"

using namespace sim;

TEST_CASE(stop_order_does_not_enter_book_immediately) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);
    engine.grant_initial_holdings(1, "AAPL", 20);

    // a SELL STOP at 90: only releases once the reference price falls to 90 or below
    // The reference price is currently 100, so it must not enter the book yet
    OrderRequest request = nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 5, 90.0);
    auto result = engine.submit_order(request);

    CHECK(result.accepted);
    CHECK(result.queued);
    CHECK_EQ(result.filled_quantity, Quantity{0});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});
    // it must not be resting in the live book either
    auto snap = engine.snapshot("AAPL", 5);
    CHECK(snap.asks.empty());
}

TEST_CASE(stop_order_releases_once_reference_price_crosses_trigger) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0); // will place the stop sell
    engine.ensure_client(2, 10000.0); // will push the price down, then buy from the released stop
    engine.grant_initial_holdings(1, "AAPL", 20);

    OrderRequest stop_request = nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 5, 90.0);
    engine.submit_order(stop_request);
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});

    // push the reference price down to 85 with an unrelated trade, crossing the stop's trigger
    engine.grant_initial_holdings(2, "AAPL", 20);
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 1, 85.0));
    engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 1, 85.0));
    CHECK_EQ(engine.last_price("AAPL"), 85.0);

    auto summary = engine.try_release_waiting_orders();
    CHECK_EQ(summary.released.size(), std::size_t{1});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{0});

    // the released stop now rests as a plain LIMIT sell at 90.0
    auto snap = engine.snapshot("AAPL", 5);
    CHECK(!snap.asks.empty());
    CHECK_EQ(snap.asks.front().price, 90.0);
}

TEST_CASE(limit_stop_releases_only_within_its_band) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);

    OrderRequest request = nrt::make_order(1, Side::BUY, OrderKind::LIMIT_STOP, "AAPL", 5, 95.0);
    request.trigger_lower = 90.0;
    request.trigger_upper = 98.0;
    auto result = engine.submit_order(request);

    CHECK(result.queued);

    // reference price (100.0) is outside [90, 98]: must still be waiting after a release scan
    // Use full_scan=true so this genuinely exercises the release-band check rather than trivially passing because a dirty-only scan never looked at this symbol at all (nothing traded here)
    auto summary = engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK_EQ(summary.released.size(), std::size_t{0});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});
}

TEST_CASE(order_with_future_not_before_stays_queued_even_if_price_matches) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);

    // a plain LIMIT order (unbounded release band, so price is never the blocker here) with a start date far in the future: it must not be released even though nothing else stops it
    OrderRequest request = nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 1, 100.0);
    request.not_before_in = std::chrono::seconds(3600);
    auto result = engine.submit_order(request);

    CHECK(result.queued);
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});

    auto summary = engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK_EQ(summary.released.size(), std::size_t{0});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});
}

TEST_CASE(waiting_order_expires_if_never_released) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);
    engine.grant_initial_holdings(1, "AAPL", 5);

    // a SELL STOP that will never trigger in this test (reference price stays at 100), with a 1-nanosecond expiry so it's already stale by the time we scan
    OrderRequest request = nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 5, 1.0);
    request.expires_in = std::chrono::seconds(0);
    engine.submit_order(request);
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // this symbol never traded, so the fast dirty-only scan (the default) would never look at it: expiry sweeps need the full scan to catch orders on otherwise-quiet symbols
    auto summary = engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK_EQ(summary.released.size(), std::size_t{0});
    CHECK_EQ(summary.expired.size(), std::size_t{1});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{0});
}

TEST_CASE(cancel_waiting_order_removes_it_before_release) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);
    engine.grant_initial_holdings(1, "AAPL", 5);

    OrderRequest request = nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 5, 90.0);
    auto result = engine.submit_order(request);
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});

    CHECK(engine.cancel_waiting_order(result.order_id));
    CHECK_EQ(engine.waiting_order_count(), std::size_t{0});
    CHECK(!engine.cancel_waiting_order(result.order_id)); // already gone
}

TEST_CASE(order_registry_scan_is_pure_and_time_driven) {
    // exercises OrderRegistry directly (not through MatchingEngine) to confirm the release scan only depends on the `now` and reference-price values it's given, not on wall-clock time
    OrderRegistry registry;

    Order order;
    order.id = 1;
    order.client = 1;
    order.side = Side::BUY;
    order.kind = OrderKind::LIMIT_STOP;
    order.symbol = "AAPL";
    order.quantity = 1;
    order.price = 100.0;
    order.release_lower = 90.0;
    order.release_upper = 110.0;
    order.submitted_at = Clock::now();
    registry.add(order);

    // reference price outside the band: nothing released
    auto scan_outside = registry.scan_and_release(Clock::now(), [](const Symbol&) { return 50.0; });
    CHECK_EQ(scan_outside.released.size(), std::size_t{0});
    CHECK_EQ(registry.size(), std::size_t{1});

    // reference price inside the band: released
    auto scan_inside = registry.scan_and_release(Clock::now(), [](const Symbol&) { return 100.0; });
    CHECK_EQ(scan_inside.released.size(), std::size_t{1});
    CHECK_EQ(registry.size(), std::size_t{0});
}

// Regression tests for side-aware STOP semantics
TEST_CASE(buy_stop_above_the_market_waits_instead_of_releasing_immediately) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);

    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::STOP, "AAPL", 5, 110.0));
    CHECK(result.accepted);
    CHECK(result.queued); // price 100 hasn't risen to 110 yet
    CHECK(engine.snapshot("AAPL", 5).bids.empty());

    auto summary = engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK_EQ(summary.released.size(), std::size_t{0});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});
}

TEST_CASE(buy_stop_releases_once_the_price_rises_to_its_trigger) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0); // places the buy stop
    engine.ensure_client(2, 100000.0); // moves the price
    engine.ensure_client(3, 100000.0);
    engine.grant_initial_holdings(2, "AAPL", 50);

    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::STOP, "AAPL", 5, 110.0));
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});

    // a trade at 112 pushes the reference price up through the 110 trigger
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 1, 112.0));
    engine.submit_order(nrt::make_order(3, Side::BUY, OrderKind::LIMIT, "AAPL", 1, 112.0));
    CHECK_EQ(engine.last_price("AAPL"), 112.0);

    auto summary = engine.try_release_waiting_orders(/*full_scan=*/false); // AAPL is dirty after that trade
    CHECK_EQ(summary.released.size(), std::size_t{1});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{0});
}

TEST_CASE(buy_stop_below_the_market_is_already_triggered) {
    // mirror image of the SELL case: price 100 is already at/above a 90 buy trigger, so it releases
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);

    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::STOP, "AAPL", 5, 90.0));
    CHECK(result.accepted);
    CHECK(!result.queued);
    CHECK_EQ(engine.waiting_order_count(), std::size_t{0});
}

TEST_CASE(sell_stop_semantics_are_unchanged_by_the_side_aware_fix) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);
    engine.grant_initial_holdings(1, "AAPL", 20);

    auto below = engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 5, 90.0));
    CHECK(below.queued); // waits for the price to fall to 90
    auto above = engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 5, 110.0));
    CHECK(!above.queued); // 100 <= 110: already triggered
}
