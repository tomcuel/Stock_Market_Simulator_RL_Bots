//=======================================================================
// Tests for the trading strategies (bot.hpp) and the ML signal (signal_model.hpp)
// Strategies are driven by calling step() directly through an InProcessGateway: deterministic, no background thread, no sleeps
// test_net_server_integration.cpp runs them over the socket protocol instead
//=======================================================================
#include <cmath>
#include <deque>
#include <fstream>

#include "bot.hpp"
#include "matching_engine.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "signal_model.hpp"
#include "test_helpers.hpp"

using namespace sim;

namespace {

// an engine with `symbols` at 100 and clients 1..n holding cash and 1000 shares of each
void seed_market(MatchingEngine& engine, const std::vector<Symbol>& symbols, int clients, double cash = 1000000.0) {
    for (const auto& s : symbols) {
        engine.register_symbol(s, 100.0);
    }
    for (ClientId id = 1; id <= clients; ++id) {
        engine.ensure_client(id, cash);
        for (const auto& s : symbols) {
            engine.grant_initial_holdings(id, s, 1000);
        }
    }
}

// one trade at `price` between two helper clients, which moves the symbol's last price
void print_price(MatchingEngine& engine, const Symbol& symbol, Price price, ClientId seller, ClientId buyer) {
    engine.submit_order(nrt::make_order(seller, Side::SELL, OrderKind::LIMIT, symbol, 1, price));
    engine.submit_order(nrt::make_order(buyer, Side::BUY, OrderKind::LIMIT, symbol, 1, price));
}

} // namespace

TEST_CASE(noise_trader_bot_trades_across_multiple_symbols_not_just_one) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    std::vector<Symbol> symbols = {"AAPL", "MSFT", "GOOG"};
    seed_market(engine, symbols, 1);
    InProcessGateway gateway(engine, 1);
    NoiseTraderBot bot(gateway, symbols, /*seed=*/42);
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
    CHECK(symbols_with_activity >= 2); // one bot, several symbols: no more "noise_IBEX" bots
}

TEST_CASE(strategies_retract_their_stale_orders) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    seed_market(engine, {"AAPL"}, 1);
    InProcessGateway gateway(engine, 1);
    NoiseTraderBot bot(gateway, {"AAPL"}, 3);
    for (int i = 0; i < 400; ++i) bot.step();

    // noise orders live at most 20 to 60 ticks: without retraction ~140 LIMIT/STOP orders would be open by now, locking the account's cash and shares
    CHECK(bot.orders_retracted() > 0);
    CHECK(bot.open_order_count() < 80);
    std::size_t in_market = engine.resting_orders("AAPL").size() + engine.waiting_orders().size();
    CHECK(in_market <= bot.open_order_count()); // everything still in the market is still tracked
}

// The regression test for "the momentum strategy doesn't place any trades": seeds a real, observable price trend via two other clients trading with each other, 
// then checks the momentum bot (sampling that trend one price per step(), same as it would over the wire) actually reacts to it with a trade
TEST_CASE(momentum_bot_reacts_to_a_significant_price_trend) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    seed_market(engine, {"AAPL"}, 3);
    InProcessGateway gateway(engine, 3);
    MomentumBot bot(gateway, {"AAPL"}, 7);

    double price = 100.0;
    OrderId resting_ask = 0;
    bool traded = false;
    for (int i = 0; i < 30 && !traded; ++i) {
        price += 2.0; // a steady, unambiguous uptrend
        if (resting_ask != 0) {
            engine.cancel_order("AAPL", resting_ask);
        }
        // liquidity at the new price for the bot to buy from, and one trade to move the last price
        resting_ask = engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 20, price)).order_id;
        engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 1, price));
        bot.step();
        traded = engine.portfolio_snapshot(3).holding("AAPL") > 1000;
    }
    CHECK(traded);
}

TEST_CASE(momentum_bot_ignores_moves_smaller_than_its_threshold) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    seed_market(engine, {"AAPL"}, 3);
    InProcessGateway gateway(engine, 3);
    MomentumBot bot(gateway, {"AAPL"}, 11);
    for (int i = 0; i < 30; ++i) {
        print_price(engine, "AAPL", 100.0 + 0.001 * i, 1, 2); // +0.03% overall: noise, not a trend
        bot.step();
    }
    CHECK_EQ(bot.orders_placed(), std::uint64_t{0});
}

TEST_CASE(market_maker_requotes_and_never_leaves_more_than_one_pair) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    std::vector<Symbol> symbols = {"AAPL", "MSFT", "GOOG"};
    seed_market(engine, symbols, 1);
    InProcessGateway gateway(engine, 1);
    MarketMakerBot bot(gateway, symbols, 9);
    for (int i = 0; i < 60; ++i) {
        bot.step();
    }

    std::size_t bids = 0, asks = 0;
    for (const auto& s : symbols) {
        auto snap = engine.snapshot(s, 1000);
        bids += snap.bids.size();
        asks += snap.asks.size();
    }
    CHECK(bids <= 1); // across all symbols: only the current pair, wherever it was last posted
    CHECK(asks <= 1);
}

TEST_CASE(market_maker_quotes_never_cross_even_with_a_large_inventory) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    seed_market(engine, {"AAPL"}, 1);
    InProcessGateway gateway(engine, 1);
    MarketMakerBot bot(gateway, {"AAPL"}, 5);
    bot.step(); // remembers 1000 shares as its target inventory
    engine.grant_initial_holdings(1, "AAPL", 5000); // then becomes massively long
    bot.step();

    // leaning to sell is fine, crossing is not: the ask must stay above the bid
    auto snap = engine.snapshot("AAPL", 5);
    CHECK(!snap.bids.empty());
    CHECK(!snap.asks.empty());
    CHECK(snap.asks.front().price > snap.bids.front().price);
    CHECK(snap.asks.front().price < 100.5); // it did lean: without the lean the ask would sit at 100.5
}

TEST_CASE(mean_reversion_bot_buys_after_a_sharp_drop) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    seed_market(engine, {"AAPL"}, 3);
    InProcessGateway gateway(engine, 3);
    MeanReversionBot bot(gateway, {"AAPL"}, 4);
    for (int i = 0; i < 25; ++i) {
        print_price(engine, "AAPL", 100.0 + ((i % 2) ? 0.2 : -0.2), 1, 2); // calm market around 100
        bot.step();
    }
    print_price(engine, "AAPL", 95.0, 1, 2); // sudden drop, far below the mean
    bot.step();

    bool has_bid = false;
    for (const auto& order : engine.resting_orders("AAPL")) {
        if (order.client == 3 && order.side == Side::BUY) {
            has_bid = true;
        }
    }
    CHECK(has_bid);
}

TEST_CASE(trend_follower_enters_on_a_crossover_and_trails_its_stop) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    seed_market(engine, {"AAPL"}, 3);
    InProcessGateway gateway(engine, 3);
    TrendFollowerBot bot(gateway, {"AAPL"}, 8);

    double price = 100.0;
    for (int i = 0; i < 25; ++i) { // flat warm-up
        print_price(engine, "AAPL", price, 1, 2);
        bot.step();
    }
    std::vector<Price> stops;
    for (int i = 0; i < 25; ++i) { // then a rally, with sellers for the bot to buy from
        price *= 1.01;
        engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 10, price));
        engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 1, price));
        bot.step();
        for (const auto& order : engine.waiting_orders()) {
            if (order.client == 3 && order.kind == OrderKind::STOP) {
                stops.push_back(order.release_upper);
            }
        }
    }
    CHECK(engine.portfolio_snapshot(3).holding("AAPL") > 1000); // it bought the uptrend
    CHECK(stops.size() >= 2);
    CHECK(!stops.empty() && stops.back() > stops.front()); // the protective stop moved up with the price
    CHECK(bot.orders_retracted() > 0);                     // by retracting and re-placing it
}

TEST_CASE(twap_bot_works_a_parent_order_to_completion) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    seed_market(engine, {"AAPL"}, 3);
    InProcessGateway maker_gateway(engine, 1);
    MarketMakerBot maker(maker_gateway, {"AAPL"}, 1); // liquidity on both sides for the TWAP to trade with
    InProcessGateway twap_gateway(engine, 3);
    TwapExecutionBot twap(twap_gateway, {"AAPL"}, 2);
    for (int i = 0; i < 200 && twap.parents_completed() == 0; ++i) {
        maker.step();
        twap.step();
    }
    CHECK(twap.parents_completed() >= 1);
    CHECK(twap.orders_placed() > 1);    // sliced into several child orders
    CHECK(twap.orders_retracted() > 0); // unfilled slices were retracted and re-planned
}

TEST_CASE(every_strategy_runs_without_overdrawing_the_account) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    std::vector<Symbol> symbols = {"AAPL", "MSFT"};
    const auto& names = strategy_names();
    seed_market(engine, symbols, static_cast<int>(names.size()), 20000.0); // little cash: many orders must be refused
    std::vector<std::unique_ptr<InProcessGateway>> gateways;
    std::vector<std::unique_ptr<TradingStrategy>> bots;
    for (std::size_t i = 0; i < names.size(); ++i) {
        gateways.push_back(std::make_unique<InProcessGateway>(engine, static_cast<ClientId>(i + 1)));
        auto bot = make_strategy(names[i], *gateways.back(), symbols, static_cast<unsigned>(i + 1));
        if (names[i] == "ml") {
            CHECK(!bot); // no model given: the factory refuses rather than building a bot that never trades
            continue;
        }
        CHECK(bot != nullptr);
        CHECK_EQ(std::string(bot->name()), names[i]);
        bots.push_back(std::move(bot));
    }
    for (int i = 0; i < 300; ++i) {
        for (auto& bot : bots) {
            bot->step();
        }
        if (i % 20 == 0) {
            engine.try_release_waiting_orders(/*full_scan=*/true);
        }
    }
    for (const auto& [client, p] : engine.all_portfolios()) {
        CHECK(p.cash >= -1e-6);
        for (const auto& s : symbols) {
            CHECK(p.holding(s) >= 0);
        }
    }
    CHECK(make_strategy("not_a_strategy", *gateways.front(), symbols, 1) == nullptr);
}

// The ML bot is trained in Python and inferred in C++: the five features must be computed the same way on both sides
// These values were computed by features_at() in scripts/train_signal_model.py for the series 100 + 3 sin(0.7 i) + 0.5 i, i = 0..19.
TEST_CASE(signal_features_match_the_python_training_code) {
    std::deque<double> prices;
    for (int i = 0; i < 20; ++i) {
        prices.push_back(100.0 + 3.0 * std::sin(i * 0.7) + 0.5 * i);
    }
    auto features = compute_signal_features(prices);
    CHECK(features.has_value());
    CHECK_NEAR((*features)[0], 0.022069853007311124, 1e-12); // ret_1
    CHECK_NEAR((*features)[1], 0.052956722193062022, 1e-12); // ret_5
    CHECK_NEAR((*features)[2], 0.037738558728800831, 1e-12); // ma_gap_10
    CHECK_NEAR((*features)[3], 0.013997298342594239, 1e-12); // vol_10
    CHECK_NEAR((*features)[4], 0.51353988887379554, 1e-12);  // rsi_14

    std::deque<double> uptrend;
    for (int i = 0; i < 20; ++i) {
        uptrend.push_back(100.0 + i);
    }
    CHECK_NEAR((*compute_signal_features(uptrend))[4], 1.0, 1e-12); // no loss at all: RSI 100

    std::deque<double> too_short(kSignalMinHistory - 1, 100.0);
    CHECK(!compute_signal_features(too_short).has_value());
}

TEST_CASE(signal_model_loads_and_predicts_like_the_formula) {
    auto dir = nrt::fresh_output_dir("signal_model");
    std::string path = (dir / "model.csv").string();
    {
        std::ofstream out(path);
        out << "# test model\nkind,name,mean,std,weight\nmeta,horizon,7,,\nbias,bias,,,0.5\nfeature,ret_1,0,1,1\nfeature,ret_5,0,1,0\nfeature,ma_gap_10,0,1,0\nfeature,vol_10,0,0,2\nfeature,rsi_14,1,2,-1\n";
    }
    SignalModel model;
    std::string error;
    CHECK(model.load(path, error));
    CHECK_EQ(model.horizon(), 7);
    // logit = 0.5 + 1 x (0.1 - 0) / 1 + 2 x (0.3 - 0) / 1 (a std of 0 counts as 1) - 1 x (0.5 - 1) / 2 = 1.45
    SignalFeatures f = {0.1, 0.0, 0.0, 0.3, 0.5};
    CHECK_NEAR(model.probability_up(f), 1.0 / (1.0 + std::exp(-1.45)), 1e-12);

    {
        std::ofstream out(path);
        out << "kind,name,mean,std,weight\nbias,bias,,,0\nfeature,ret_5,0,1,1\n";
    }
    SignalModel wrong_order;
    CHECK(!wrong_order.load(path, error)); // features out of order are refused, not silently misread
    CHECK(error.find("ret_5") != std::string::npos);
    CHECK(!SignalModel().load((dir / "missing.csv").string(), error));
}

namespace {

// exposes the protected lot sizing of a strategy for the test below
struct LotProbe : public NoiseTraderBot
{
    using NoiseTraderBot::NoiseTraderBot;
    Quantity probe(Price price, double units) const {return lots(price, units);}
    double probe_lot_value() const {return lot_value();}
};

} // namespace

// Orders are sized by value: one lot is 0.2% of the bot's starting equity whatever the symbol's
// price. With share counts, "10 shares" was 8700 on one index and 425000 on another.
TEST_CASE(orders_are_sized_by_value_not_share_count) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("CHEAP", 10.0);
    engine.register_symbol("DJI", 42573.73);
    engine.fund_balanced_account(1, 10000000.0, 0.5);
    InProcessGateway gateway(engine, 1);
    LotProbe bot(gateway, {"CHEAP", "DJI"}, 1);
    bot.step(); // the first tick measures the equity and sets the lot value

    CHECK_NEAR(bot.probe_lot_value(), 20000.0, 1.0);  // 0.2% of 10 million
    CHECK_EQ(bot.probe(10.0, 1.0), Quantity{2000});    // one lot of a 10 symbol: 2000 shares
    CHECK_EQ(bot.probe(4000.0, 5.0), Quantity{25});    // five lots at 4000: 25 shares, the same value per lot
    CHECK_EQ(bot.probe(42573.73, 1.0), Quantity{1});   // under one share: never less than 1
    CHECK_EQ(bot.probe(42573.73, 10.0), Quantity{5});  // ten lots: 200000 of value, about 5 shares
}

