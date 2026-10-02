// Tests the in-process bot strategies directly, by calling step() (public specifically so it's testable without a real background thread and real sleeps) against a real MatchingEngine
// no sockets involved here, see test_net_server_integration.cpp for the wire-protocol equivalent
#include "bot.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"

using namespace sim;

TEST_CASE(noise_trader_bot_trades_across_multiple_symbols_not_just_one) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    MarketData market_data(bus);
    std::vector<Symbol> symbols = {"AAPL", "MSFT", "GOOG"};
    for (const auto& symbol : symbols) {
        engine.register_symbol(symbol, 100.0);
    }
    engine.ensure_client(1, 1000000.0);
    for (const auto& symbol : symbols) {
        engine.grant_initial_holdings(1, symbol, 1000);
    }

    NoiseTraderBot bot(engine, market_data, 1, symbols, std::chrono::milliseconds(1), std::chrono::milliseconds(1), /*seed=*/42);
    for (int i = 0; i < 300; ++i) {
        bot.step();
    }

    // this is the core regression test for "there should not be a noise_IBEX bot": 
    // a single bot given multiple symbols must actually visit more than one of them, not settle on whichever it picked first
    //checked here by looking for resting orders (LIMIT/STOP/LIMIT_STOP release into the book, MARKET doesn't rest) left behind on at least 2 distinct symbols
    int symbols_with_activity = 0;
    for (const auto& symbol : symbols) {
        auto snap = engine.snapshot(symbol, 100);
        if (!snap.bids.empty() || !snap.asks.empty()) {
            ++symbols_with_activity;
        }
    }
    CHECK(symbols_with_activity >= 2);
}

// The regression test for "the momentum strategy doesn't place any trades": seeds a real, observable price trend via two other clients trading with each other, 
// then checks the momentum bot (sampling that trend one price per step(), same as it would over the wire) actually reacts to it with a trade
TEST_CASE(momentum_bot_reacts_to_a_real_price_trend) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    MarketData market_data(bus);
    engine.register_symbol("AAPL", 100.0);

    ClientId price_setter = 1, seller = 2, momentum_client = 3;
    engine.ensure_client(price_setter, 1000000.0);
    engine.ensure_client(seller, 1000000.0);
    engine.ensure_client(momentum_client, 1000000.0);
    engine.grant_initial_holdings(seller, "AAPL", 10000); // plenty to keep posting resting liquidity
    engine.grant_initial_holdings(momentum_client, "AAPL", 1000); // so it can also SELL if it trends down

    MomentumBot bot(engine, market_data, momentum_client, {"AAPL"}, std::chrono::milliseconds(1), std::chrono::milliseconds(1), /*seed=*/7);

    double price = 100.0;
    OrderId last_seller_order_id = 0;
    bool momentum_traded = false;
    for (int i = 0; i < 10 && !momentum_traded; ++i) {
        price += 2.0; // a steady, unambiguous uptrend

        if (last_seller_order_id != 0) {
            engine.cancel_order("AAPL", last_seller_order_id);
        }

        // seller posts fresh resting liquidity at the new price; price_setter takes 1 share to move last_price, 
        // leaving the rest resting so momentum's own MARKET buy (IOC) has something to actually fill against a moment later, instead of finding zero liquidity
        auto sell_result = engine.submit_order(nrt::make_order(seller, Side::SELL, OrderKind::LIMIT, "AAPL", 20, price));
        last_seller_order_id = sell_result.order_id;
        engine.submit_order(nrt::make_order(price_setter, Side::BUY, OrderKind::LIMIT, "AAPL", 1, price));

        bot.step(); // samples the just-updated price into its own rolling window

        Portfolio p = engine.portfolio_snapshot(momentum_client);
        if (p.holding("AAPL") != 1000 || p.cash < 999999.0) {
            momentum_traded = true; // its holdings or cash moved: it placed and filled an order
        }
    }

    CHECK(momentum_traded);
}

TEST_CASE(momentum_bot_places_no_trade_before_it_has_two_samples) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    MarketData market_data(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000000.0);
    engine.grant_initial_holdings(1, "AAPL", 1000);

    MomentumBot bot(engine, market_data, 1, {"AAPL"}, std::chrono::milliseconds(1), std::chrono::milliseconds(1), 3);

    Portfolio before = engine.portfolio_snapshot(1);
    bot.step(); // exactly one sample so far: must not act on a single data point
    Portfolio after = engine.portfolio_snapshot(1);

    CHECK_EQ(before.holding("AAPL"), after.holding("AAPL"));
    CHECK_NEAR(before.cash, after.cash, 1e-9);
}

TEST_CASE(momentum_bot_keeps_separate_trend_windows_per_symbol) {
    // if the per-symbol price history were accidentally shared across symbols (e.g. one flat deque instead of a map), 
    // an AAPL price rise immediately followed by a single MSFT sample would look like "MSFT is trending up" even though MSFT itself never moved
    NotificationBus bus;
    MatchingEngine engine(bus);
    MarketData market_data(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.register_symbol("MSFT", 300.0);
    ClientId buyer = 1, seller = 2, momentum_client = 3;
    engine.ensure_client(buyer, 1000000.0);
    engine.ensure_client(seller, 1000000.0);
    engine.ensure_client(momentum_client, 1000000.0);
    engine.grant_initial_holdings(seller, "AAPL", 1000);
    engine.grant_initial_holdings(momentum_client, "MSFT", 1000);

    MomentumBot bot(engine, market_data, momentum_client, {"AAPL", "MSFT"}, std::chrono::milliseconds(1), std::chrono::milliseconds(1), 11);

    // push AAPL up first (bot may or may not happen to sample AAPL on these steps, that's fine)
    for (int i = 0; i < 3; ++i) {
        double price = 100.0 + i * 5.0;
        engine.submit_order(nrt::make_order(seller, Side::SELL, OrderKind::LIMIT, "AAPL", 1, price));
        engine.submit_order(nrt::make_order(buyer, Side::BUY, OrderKind::LIMIT, "AAPL", 1, price));
    }

    // MSFT itself never moves from 300.0 for the whole test: so however the bot's rotation happens to land on MSFT, it must never see "2+ different MSFT samples with a nonzero change"
    Portfolio before_msft = engine.portfolio_snapshot(momentum_client);
    for (int i = 0; i < 20; ++i) bot.step();
    Portfolio after_msft = engine.portfolio_snapshot(momentum_client);

    // the bot may well have traded AAPL (that's fine and expected): what must not happen is MSFT holdings moving, since MSFT's own price history is flat the entire time
    CHECK_EQ(before_msft.holding("MSFT"), after_msft.holding("MSFT"));
}

TEST_CASE(market_maker_bot_cancels_previous_quotes_before_requoting) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    MarketData market_data(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 1000000.0);
    engine.grant_initial_holdings(1, "AAPL", 1000);

    MarketMakerBot bot(engine, market_data, 1, {"AAPL"}, std::chrono::milliseconds(1), std::chrono::milliseconds(1), /*seed=*/3);
    for (int i = 0; i < 25; ++i) bot.step();

    // the regression this pins down: without cancel-and-requote, 25 ticks x 2 quotes each would leave up to 50 resting orders (or at least many distinct price levels) behind
    // with it, at most one bid and one ask survive at any given time
    auto snap = engine.snapshot("AAPL", 1000);
    CHECK(snap.bids.size() <= 1);
    CHECK(snap.asks.size() <= 1);
}

TEST_CASE(market_maker_bot_cancels_on_the_right_symbol_when_it_rotates) {
    // the specific bug this guards against: MarketMakerBot must remember which symbol its last  quotes were posted on (last_quote_symbol_) and cancel there, 
    // not on whatever symbol it happens to pick next: otherwise rotating across symbols would leave stale quotes behind on every symbol it ever visited
    NotificationBus bus;
    MatchingEngine engine(bus);
    MarketData market_data(bus);
    std::vector<Symbol> symbols = {"AAPL", "MSFT", "GOOG"};
    for (const auto& symbol : symbols) engine.register_symbol(symbol, 100.0);
    engine.ensure_client(1, 1000000.0);
    for (const auto& symbol : symbols) engine.grant_initial_holdings(1, symbol, 1000);

    MarketMakerBot bot(engine, market_data, 1, symbols, std::chrono::milliseconds(1), std::chrono::milliseconds(1), /*seed=*/9);
    for (int i = 0; i < 60; ++i) bot.step();

    // across ALL symbols combined, at most one bid + one ask should ever remain resting (the bot's single currently-active quote pair, wherever it last landed)
    // not one leftover pair per symbol it ever visited along the way
    std::size_t total_bid_levels = 0, total_ask_levels = 0;
    for (const auto& symbol : symbols) {
        auto snap = engine.snapshot(symbol, 1000);
        total_bid_levels += snap.bids.size();
        total_ask_levels += snap.asks.size();
    }
    CHECK(total_bid_levels <= 1);
    CHECK(total_ask_levels <= 1);
}
