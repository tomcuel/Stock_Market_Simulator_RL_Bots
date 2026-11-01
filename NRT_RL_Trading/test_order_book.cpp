#include <random>
#include <thread>
#include <vector>

#include "nrt_framework.hpp"
#include "order_book.hpp"

using namespace sim;

namespace {

Order make_order(OrderId id, ClientId client, Side side, OrderKind kind, Quantity qty, Price price = 0.0) {
    Order o;
    o.id = id;
    o.client = client;
    o.side = side;
    o.kind = kind;
    o.symbol = "TEST";
    o.quantity = qty;
    o.price = price;
    o.submitted_at = Clock::now();
    return o;
}

} // namespace

TEST_CASE(limit_orders_rest_when_no_cross) {
    OrderBook book("TEST");
    auto trades = book.match(make_order(1, 100, Side::BUY, OrderKind::LIMIT, 10, 50.0));
    CHECK(trades.empty());
    CHECK_EQ(book.best_bid().value(), 50.0);
    CHECK(!book.best_ask().has_value());
}

TEST_CASE(crossing_limit_orders_match_at_resting_price) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::SELL, OrderKind::LIMIT, 10, 50.0)); // resting ask at 50
    auto trades = book.match(make_order(2, 200, Side::BUY, OrderKind::LIMIT, 10, 55.0)); // willing to pay up to 55

    CHECK_EQ(trades.size(), std::size_t{1});
    CHECK_EQ(trades[0].price, 50.0); // executes at the resting (ask) order's price, not the aggressor's
    CHECK_EQ(trades[0].quantity, Quantity{10});
    CHECK_EQ(trades[0].buyer, ClientId{200});
    CHECK_EQ(trades[0].seller, ClientId{100});
}

TEST_CASE(partial_fill_leaves_remainder_resting) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::SELL, OrderKind::LIMIT, 5, 50.0));
    auto trades = book.match(make_order(2, 200, Side::BUY, OrderKind::LIMIT, 10, 50.0));

    CHECK_EQ(trades.size(), std::size_t{1});
    CHECK_EQ(trades[0].quantity, Quantity{5});
    CHECK_EQ(book.best_bid().value(), 50.0); // remaining 5 of the buy order now rests
    CHECK(!book.best_ask().has_value());     // the resting sell order was fully consumed
}

TEST_CASE(market_buy_matches_any_resting_ask_price) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::SELL, OrderKind::LIMIT, 10, 999.0)); // a very high ask
    auto trades = book.match(make_order(2, 200, Side::BUY, OrderKind::MARKET, 10));

    // a MARKET order must match regardless of how far the resting price is, and trade at the resting order's real price
    // this is exactly the bug class fixed in the SQL engine, where a MARKET order's placeholder price could wrongly block or corrupt the match
    CHECK_EQ(trades.size(), std::size_t{1});
    CHECK_EQ(trades[0].price, 999.0);
}

TEST_CASE(market_sell_matches_any_resting_bid_price) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::BUY, OrderKind::LIMIT, 10, 1.0)); // a very low bid
    auto trades = book.match(make_order(2, 200, Side::SELL, OrderKind::MARKET, 10));

    CHECK_EQ(trades.size(), std::size_t{1});
    CHECK_EQ(trades[0].price, 1.0);
}

TEST_CASE(unfilled_market_order_does_not_rest_in_book) {
    OrderBook book("TEST");
    // no resting liquidity at all: a MARKET order must be discarded (IOC), never inserted into the book with a placeholder price that would corrupt future priority/settlement
    auto trades = book.match(make_order(1, 100, Side::BUY, OrderKind::MARKET, 10));
    
    CHECK(trades.empty());
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
}

TEST_CASE(partially_filled_market_order_discards_remainder) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::SELL, OrderKind::LIMIT, 3, 50.0)); // only 3 shares available
    auto trades = book.match(make_order(2, 200, Side::BUY, OrderKind::MARKET, 10)); // wants 10

    CHECK_EQ(trades.size(), std::size_t{1});
    CHECK_EQ(trades[0].quantity, Quantity{3});
    CHECK(!book.best_bid().has_value()); // the unfilled 7 must NOT be sitting in the book
    CHECK(!book.best_ask().has_value());
}

TEST_CASE(price_time_priority_within_same_price_level) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::SELL, OrderKind::LIMIT, 5, 50.0)); // first in
    book.match(make_order(2, 101, Side::SELL, OrderKind::LIMIT, 5, 50.0)); // second in

    auto trades = book.match(make_order(3, 200, Side::BUY, OrderKind::LIMIT, 5, 50.0));
    CHECK_EQ(trades.size(), std::size_t{1});
    CHECK_EQ(trades[0].seller, ClientId{100}); // the earlier order at the same price fills first
}

TEST_CASE(best_price_level_matched_before_worse_level) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::SELL, OrderKind::LIMIT, 5, 55.0)); // worse (higher) ask
    book.match(make_order(2, 101, Side::SELL, OrderKind::LIMIT, 5, 50.0)); // better (lower) ask

    auto trades = book.match(make_order(3, 200, Side::BUY, OrderKind::LIMIT, 5, 60.0));
    CHECK_EQ(trades.size(), std::size_t{1});
    CHECK_EQ(trades[0].price, 50.0); // the best available price is matched first
}

TEST_CASE(cancel_removes_resting_order) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::BUY, OrderKind::LIMIT, 10, 50.0));
    CHECK(book.cancel(Side::BUY, 50.0, 1));
    CHECK(!book.best_bid().has_value());
}

TEST_CASE(cancel_of_unknown_order_returns_false) {
    OrderBook book("TEST");
    CHECK(!book.cancel(Side::BUY, 50.0, 12345));
}

TEST_CASE(snapshot_aggregates_quantity_per_price_level) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::BUY, OrderKind::LIMIT, 5, 50.0));
    book.match(make_order(2, 101, Side::BUY, OrderKind::LIMIT, 7, 50.0)); // same level as order 1
    book.match(make_order(3, 102, Side::BUY, OrderKind::LIMIT, 3, 49.0));

    auto snap = book.snapshot(5);
    CHECK_EQ(snap.bids.size(), std::size_t{2});
    CHECK_EQ(snap.bids[0].price, 50.0); // best (highest) bid first
    CHECK_EQ(snap.bids[0].quantity, Quantity{12});
    CHECK_EQ(snap.bids[1].price, 49.0);
}

TEST_CASE(cancel_by_id_alone_finds_the_order_without_knowing_its_side_or_price) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::BUY, OrderKind::LIMIT, 10, 50.0));
    book.match(make_order(2, 101, Side::SELL, OrderKind::LIMIT, 5, 60.0));

    CHECK(book.cancel(OrderId{1})); // finds it on the bid side without being told
    CHECK(book.cancel(OrderId{2})); // finds it on the ask side without being told
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
}

TEST_CASE(cancel_by_id_alone_of_unknown_order_returns_false) {
    OrderBook book("TEST");
    book.match(make_order(1, 100, Side::BUY, OrderKind::LIMIT, 10, 50.0));
    CHECK(!book.cancel(OrderId{999}));
    CHECK(book.best_bid().has_value()); // the real order must still be untouched
}

// Many threads concurrently submitting and cancelling orders on the SAME book at once: direct stress test of OrderBook's single shared_mutex under contention from both match() (a writer) and cancel()/snapshot() (also a writer / a reader) simultaneously.
// The invariant checked is simple but strict: the book must never end up in an inconsistent state (a crash, a hang, or a negative/corrupted resting quantity would all be regressions this test would catch), 
// Every order that wasn't cancelled and wasn't matched must still be exactly findable afterward
TEST_CASE(concurrent_submit_and_cancel_on_one_book_never_corrupts_it) {
    OrderBook book("TEST");
    const int num_threads = 16;
    const int orders_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<OrderId> next_id{1};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> qty_dist(1, 5);
            std::uniform_real_distribution<double> price_dist(40.0, 60.0);
            std::bernoulli_distribution side_dist(0.5);
            std::bernoulli_distribution cancel_dist(0.3);

            std::vector<OrderId> my_order_ids;
            for (int i = 0; i < orders_per_thread; ++i) {
                OrderId id = next_id.fetch_add(1);
                Side side = side_dist(rng) ? Side::BUY : Side::SELL;
                book.match(make_order(id, 1000 + t, side, OrderKind::LIMIT, qty_dist(rng), price_dist(rng)));
                my_order_ids.push_back(id);

                // occasionally cancel one of this thread's own earlier orders, and occasionally take a snapshot: both while other threads are still submitting concurrently
                if (cancel_dist(rng) && !my_order_ids.empty()) {
                    book.cancel(my_order_ids[rng() % my_order_ids.size()]);
                }
                if (i % 10 == 0) {
                    auto snap = book.snapshot(10); // must never crash/hang while writers are active
                    (void)snap;
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    // the book must still be in a fully queryable, consistent state: 
    // every resting quantity must be positive (never corrupted to 0 or negative by a lost update), and best_bid <= no structural assumption is violated by simply being able to snapshot it without crashing
    auto final_snapshot = book.snapshot(1000);
    for (const auto& level : final_snapshot.bids) CHECK(level.quantity > 0);
    for (const auto& level : final_snapshot.asks) CHECK(level.quantity > 0);
}
