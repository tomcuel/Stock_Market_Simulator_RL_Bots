//=======================================================================
// Tests for reservations: open orders are not debited (the client still owns the engaged cash and shares) but they are reserved, so the same money can never be promised to two orders at once
// Found in a real run: one bot ended at -18768.89 cash because several resting buys had each been checked against the whole balance and then all filled
//=======================================================================
#include <random>
#include <thread>
#include <vector>

#include "matching_engine.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"

using namespace sim;

TEST_CASE(open_buy_orders_cannot_over_commit_cash) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000.0);
    engine.ensure_client(2, 0.0);
    engine.grant_initial_holdings(2, "AAPL", 100);

    CHECK(engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 100.0)).accepted); // engages all 1000
    auto second = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 100.0));
    CHECK(!second.accepted);
    CHECK_EQ(second.reject_reason, std::string("insufficient cash"));

    // the open order is not debited: the cash is still owned, only reserved
    Portfolio before_fill = engine.portfolio_snapshot(1);
    CHECK_NEAR(before_fill.cash, 1000.0, 1e-9);
    CHECK_NEAR(before_fill.reserved_cash, 1000.0, 1e-9);
    CHECK_NEAR(before_fill.available_cash(), 0.0, 1e-9);

    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 30, 100.0));
    Portfolio after_fill = engine.portfolio_snapshot(1);
    CHECK_NEAR(after_fill.cash, 0.0, 1e-9); // was -2000 before reservations existed
    CHECK_NEAR(after_fill.reserved_cash, 0.0, 1e-9);
    CHECK_EQ(after_fill.holding("AAPL"), Quantity{10});
}

TEST_CASE(open_sell_orders_cannot_over_commit_shares) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 0.0);
    engine.grant_initial_holdings(1, "AAPL", 10);

    CHECK(engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 110.0)).accepted);
    auto second = engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 5, 90.0)); // a waiting order reserves too
    CHECK(!second.accepted);
    CHECK_EQ(second.reject_reason, std::string("insufficient holdings"));
    CHECK_EQ(engine.portfolio_snapshot(1).holding("AAPL"), Quantity{10}); // still owned
    CHECK_EQ(engine.portfolio_snapshot(1).reserved("AAPL"), Quantity{10});
}

TEST_CASE(cancel_releases_the_reservation) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000.0);

    auto first = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 90.0));
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 900.0, 1e-9);
    CHECK(engine.cancel_order("AAPL", first.order_id));
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 0.0, 1e-9);
    CHECK(engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 95.0)).accepted); // cash usable again
}

TEST_CASE(cancelling_a_waiting_order_releases_its_reservation) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 0.0);
    engine.grant_initial_holdings(1, "AAPL", 20);

    auto stop = engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::STOP, "AAPL", 20, 90.0));
    CHECK(stop.queued);
    CHECK_EQ(engine.portfolio_snapshot(1).reserved("AAPL"), Quantity{20});
    CHECK(engine.cancel_waiting_order(stop.order_id));
    CHECK_EQ(engine.portfolio_snapshot(1).reserved("AAPL"), Quantity{0});
}

TEST_CASE(an_expired_waiting_order_releases_its_reservation) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000.0);

    OrderRequest request = nrt::make_order(1, Side::BUY, OrderKind::STOP, "AAPL", 5, 150.0); // waits for 150
    request.expires_in = std::chrono::seconds(0);
    CHECK(engine.submit_order(request).queued);
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 750.0, 1e-9);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto summary = engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK_EQ(summary.expired.size(), std::size_t{1});
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 0.0, 1e-9);
}

TEST_CASE(partial_fill_keeps_the_remainder_reserved_and_frees_the_price_improvement) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 10000.0);
    engine.ensure_client(2, 0.0);
    engine.grant_initial_holdings(2, "AAPL", 4);

    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 4, 95.0)); // cheaper than the buyer's limit
    auto buy = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 100.0));
    CHECK_EQ(buy.filled_quantity, Quantity{4});

    Portfolio p = engine.portfolio_snapshot(1);
    CHECK_NEAR(p.cash, 10000.0 - 4 * 95.0, 1e-9);   // paid the trade price
    CHECK_NEAR(p.reserved_cash, 6 * 100.0, 1e-9);   // only the 6 still resting stay reserved, at their limit
    CHECK_NEAR(p.available_cash(), p.cash - 600.0, 1e-9);
}

TEST_CASE(market_buy_respects_its_collar_and_frees_the_unfilled_part) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);
    engine.ensure_client(2, 0.0);
    engine.grant_initial_holdings(2, "AAPL", 10);

    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 105.0)); // inside the +10% collar
    engine.submit_order(nrt::make_order(2, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 125.0)); // outside it
    auto buy = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::MARKET, "AAPL", 10));

    CHECK_EQ(buy.filled_quantity, Quantity{5});                        // stopped at the collar (110)
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 0.0, 1e-9); // IOC: the unfilled 5 released
    CHECK(!engine.snapshot("AAPL", 5).asks.empty());                   // the 125 ask is still there
}

// Many clients trading concurrently with every order kind, including cancels
// However threads interleave, nobody may end with negative cash or shares, reservations may never go negative, and once every open order is cancelled, nothing may stay reserved
TEST_CASE(concurrent_trading_never_overdraws_and_reservations_return_to_zero) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    std::vector<Symbol> symbols = {"AAPL", "MSFT"};
    for (const auto& s : symbols) {
        engine.register_symbol(s, 100.0);
    }
    const int num_clients = 12;
    for (ClientId id = 1; id <= num_clients; ++id) {
        engine.ensure_client(id, 5000.0); // deliberately small: plenty of orders must be refused
        for (const auto& s : symbols) engine.grant_initial_holdings(id, s, 30);
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < num_clients; ++t) {
        threads.emplace_back([&, t]{
            std::mt19937 rng(t + 7);
            std::uniform_int_distribution<int> qty_dist(1, 15);
            std::uniform_real_distribution<double> price_dist(85.0, 115.0);
            std::uniform_int_distribution<int> kind_dist(0, 3);
            std::bernoulli_distribution side_dist(0.5);
            std::vector<std::pair<Symbol, OrderId>> mine;
            for (int i = 0; i < 300; ++i) {
                const Symbol& symbol = symbols[i % symbols.size()];
                OrderRequest request = nrt::make_order(t + 1, side_dist(rng) ? Side::BUY : Side::SELL, static_cast<OrderKind>(kind_dist(rng)), symbol, qty_dist(rng), price_dist(rng));
                auto result = engine.submit_order(request);
                if (result.accepted) mine.emplace_back(symbol, result.order_id);
                if (i % 7 == 0 && !mine.empty()) {
                    auto& [s, id] = mine[rng() % mine.size()];
                    engine.cancel_order(s, id);
                    engine.cancel_waiting_order(id);
                }
                if (i % 25 == 0) {
                    engine.try_release_waiting_orders(/*full_scan=*/true);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    for (const auto& [client, p] : engine.all_portfolios()) {
        CHECK(p.cash >= -1e-6);
        CHECK(p.reserved_cash >= 0.0);
        for (const auto& s : symbols) {
            CHECK(p.holding(s) >= 0);
            CHECK(p.reserved(s) >= 0);
            CHECK(p.reserved(s) <= p.holding(s));
        }
    }

    // cancel everything still open: every reservation must come back to (floating point) zero
    for (const auto& s : symbols) {
        for (const auto& order : engine.resting_orders(s)) {
            engine.cancel_order(s, order.id);
        }
    }
    for (const auto& order : engine.waiting_orders()) {
        engine.cancel_waiting_order(order.id);
    }
    double total_cash = 0.0;
    for (const auto& [client, p] : engine.all_portfolios()) {
        CHECK_NEAR(p.reserved_cash, 0.0, 1e-6);
        for (const auto& s : symbols) {
            CHECK_EQ(p.reserved(s), Quantity{0});
        }
        total_cash += p.cash;
    }
    CHECK_NEAR(total_cash, 5000.0 * num_clients, 1e-6); // and cash was only ever moved, never created
}
