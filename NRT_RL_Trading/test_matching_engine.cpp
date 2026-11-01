#include <cmath>

#include "matching_engine.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"
#include "notification.hpp"

using namespace sim;

TEST_CASE(unknown_symbol_is_rejected) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.ensure_client(1, 1000.0);

    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::MARKET, "NOPE", 1));
    CHECK(!result.accepted);
    CHECK_EQ(result.reject_reason, std::string("unknown symbol"));
}

TEST_CASE(buy_without_enough_cash_is_rejected) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 50.0); // not enough for even 1 share at 100

    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 1, 100.0));
    CHECK(!result.accepted);
    CHECK_EQ(result.reject_reason, std::string("insufficient cash"));
}

TEST_CASE(sell_without_enough_holdings_is_rejected) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000.0); // cash isn't the issue, holdings are

    auto result = engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 100.0));
    CHECK(!result.accepted);
    CHECK_EQ(result.reject_reason, std::string("insufficient holdings"));
}

TEST_CASE(matched_trade_settles_both_portfolios) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0); // buyer
    engine.ensure_client(2, 0.0);     // seller
    engine.grant_initial_holdings(2, "AAPL", 20);

    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 100.0));
    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 100.0));

    CHECK(result.accepted);
    CHECK_EQ(result.filled_quantity, Quantity{10});

    Portfolio buyer = engine.portfolio_snapshot(1);
    Portfolio seller = engine.portfolio_snapshot(2);

    CHECK_NEAR(buyer.cash, 9000.0, 1e-9); // 10000 - 10*100
    CHECK_EQ(buyer.holding("AAPL"), Quantity{10});

    CHECK_NEAR(seller.cash, 1000.0, 1e-9); // 0 + 10*100
    CHECK_EQ(seller.holding("AAPL"), Quantity{10}); // 20 - 10
}

TEST_CASE(last_price_updates_after_a_trade) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);
    engine.ensure_client(2, 0.0);
    engine.grant_initial_holdings(2, "AAPL", 20);

    CHECK_EQ(engine.last_price("AAPL"), 100.0); // reference price before any trade

    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 123.5));
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 123.5));

    CHECK_EQ(engine.last_price("AAPL"), 123.5);
}

TEST_CASE(zero_or_negative_quantity_is_rejected) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);

    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::MARKET, "AAPL", 0));
    CHECK(!result.accepted);
}

TEST_CASE(limit_order_without_positive_price_is_rejected) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);

    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 1, 0.0));
    CHECK(!result.accepted);
}

TEST_CASE(notification_bus_delivers_trade_events) {
    // trade_events must be declared (and therefore constructed) before bus, so that (by reverse destruction order) it is destroyed AFTER bus, not before
    // NotificationBus's worker thread can still be invoking this capturing lambda asynchronously: bus's destructor is what guarantees "no more callback invocations after this returns" (it stops and joins the worker thread), so that must complete before trade_events' storage goes away, not after
    std::atomic<int> trade_events{0};
    NotificationBus bus;
    bus.subscribe([&](const Event& event) {
        if (std::get_if<TradeEvent>(&event)) {
            trade_events.fetch_add(1);
        }
    });

    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);
    engine.ensure_client(2, 0.0);
    engine.grant_initial_holdings(2, "AAPL", 20);

    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 100.0));
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 100.0));

    // the notification worker thread runs asynchronously: give it a moment to drain the queue
    for (int i = 0; i < 100 && trade_events.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_EQ(trade_events.load(), 1);
}

TEST_CASE(all_portfolios_returns_every_client) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.ensure_client(1, 1000.0);
    engine.ensure_client(2, 2000.0);
    engine.ensure_client(3, 3000.0);

    auto portfolios = engine.all_portfolios();
    CHECK_EQ(portfolios.size(), std::size_t{3});
    CHECK_NEAR(portfolios.at(1).cash, 1000.0, 1e-9);
    CHECK_NEAR(portfolios.at(2).cash, 2000.0, 1e-9);
    CHECK_NEAR(portfolios.at(3).cash, 3000.0, 1e-9);
}

TEST_CASE(cancel_order_by_id_alone_works_through_the_engine) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);

    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 5, 90.0));
    CHECK(result.accepted);

    CHECK(engine.cancel_order("AAPL", result.order_id)); // no side/price needed
    auto snap = engine.snapshot("AAPL", 5);
    CHECK(snap.bids.empty());
}

TEST_CASE(dirty_scan_only_rechecks_symbols_that_actually_traded) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.register_symbol("MSFT", 100.0);
    engine.ensure_client(1, 100000.0);
    engine.ensure_client(2, 100000.0);
    engine.grant_initial_holdings(2, "AAPL", 50);

    // a LIMIT order on MSFT whose price band is already satisfied by MSFT's reference price, but with a `not_before` start date a little in the future: so it's held in the waiting registry purely on account of its start date 
    // No MSFT trade is ever needed (or expected) to release it once that date passes, which is exactly the scenario a price-driven dirty scan can never catch on its own
    OrderRequest msft_request = nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "MSFT", 1, 100.0);
    msft_request.not_before_in = std::chrono::seconds(1);
    auto msft_result = engine.submit_order(msft_request);
    CHECK(msft_result.queued);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // let the start date pass

    // AAPL trades, marking AAPL (not MSFT) dirty
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 100.0));
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 5, 100.0));

    auto dirty_summary = engine.try_release_waiting_orders(/*full_scan=*/false);
    CHECK_EQ(dirty_summary.released.size(), std::size_t{0}); // MSFT's order wasn't even looked at, despite its start date having passed
    CHECK_EQ(engine.waiting_order_count(), std::size_t{1});

    // a full scan, by contrast, must catch it once its start date has passed
    auto full_summary = engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK_EQ(full_summary.released.size(), std::size_t{1});
    CHECK_EQ(engine.waiting_order_count(), std::size_t{0});
}

TEST_CASE(a_trade_marks_its_symbol_dirty_so_the_next_dirty_scan_catches_its_own_waiting_orders) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);
    engine.ensure_client(2, 100000.0);
    engine.grant_initial_holdings(1, "AAPL", 10); // client 1 needs shares to place a SELL STOP at all
    engine.grant_initial_holdings(2, "AAPL", 50);

    // queue a SELL STOP that isn't release-ready yet at the current reference price (100)
    auto stop_result = engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 1, 90.0));
    CHECK(stop_result.queued);

    // push the price down through the trigger with a real trade: this must mark AAPL dirty
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 85.0));
    engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 5, 85.0));

    // a dirty-only scan (the default, and what the hot path uses) must now find and release it, with no need for a full scan
    auto summary = engine.try_release_waiting_orders(/*full_scan=*/false);
    CHECK_EQ(summary.released.size(), std::size_t{1});
}

// Balanced funding: every account gets the same VALUE of each symbol, not the same number of shares
// With 100 shares of everything, index-priced symbols made bots 98% stock and 2% cash: they could sell everything and buy almost nothing, and every price drifted down
TEST_CASE(balanced_funding_gives_every_symbol_the_same_value) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("CHEAP", 10.0);
    engine.register_symbol("MID", 500.0);
    engine.register_symbol("DJI", 42573.73);
    double stock = engine.fund_balanced_account(1, 10000000.0, 0.5);

    Portfolio p = engine.portfolio_snapshot(1);
    auto prices = engine.all_last_prices();
    double budget = 5000000.0 / 3.0;
    const std::vector<Symbol> symbols = {"CHEAP", "MID", "DJI"};
    for (const auto& s : symbols) {
        double value = static_cast<double>(p.holding(s)) * prices[s];
        CHECK(std::abs(value - budget) <= prices[s]); // within one share of an equal split
    }
    CHECK_NEAR(p.cash + stock, 10000000.0, 1e-6);  // rounding the shares never changes the equity
    CHECK_NEAR(p.net_worth(prices), 10000000.0, 1e-6);
    CHECK(p.cash > 4900000.0 && p.cash < 5100000.0);
}

TEST_CASE(balanced_funding_skips_a_symbol_too_expensive_for_its_budget) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("A", 100.0);
    engine.register_symbol("HUGE", 200000.0);
    engine.fund_balanced_account(1, 100000.0, 0.5); // 25000 per symbol: one HUGE share would be 8 times that

    Portfolio p = engine.portfolio_snapshot(1);
    CHECK_EQ(p.holding("A"), Quantity{250});
    CHECK_EQ(p.holding("HUGE"), Quantity{0});
    CHECK_NEAR(p.cash, 75000.0, 1e-6); // the unspent budget stays in cash
}
