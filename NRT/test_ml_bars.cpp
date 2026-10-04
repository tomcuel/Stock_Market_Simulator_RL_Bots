//=======================================================================
// Tests for the ML bot's bars and positions (MLSignalBot, bot.hpp): bars on a grid shared by every bot, one position per
// symbol, closed after the model's horizon unless its protective stop fired first
//=======================================================================
#include <chrono>
#include <fstream>
#include <thread>

#include "bot.hpp"
#include "gateway.hpp"
#include "matching_engine.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "signal_model.hpp"
#include "test_helpers.hpp"

using namespace sim;

namespace {

constexpr auto kBar = std::chrono::milliseconds(20);

// a model that always says "up" (a huge bias, no feature weight), horizon 3 bars
std::shared_ptr<const SignalModel> always_up_model() {
    auto dir = nrt::fresh_output_dir("ml_bars_model");
    std::string path = (dir / "model.csv").string();
    {
        std::ofstream out(path);
        out << "kind,name,mean,std,weight\nmeta,horizon,3,,\nbias,bias,,,20\n";
        for (const char* f : {"ret_1", "ret_5", "ma_gap_10", "vol_10", "rsi_14"}) out << "feature," << f << ",0,1,0\n";
    }
    auto model = std::make_shared<SignalModel>();
    std::string error;
    model->load(path, error);
    return model;
}

// AAPL at 100, client 1 sells at 100.5 and client 2 buys at `bid` (plenty of both), the bot is client 3
void market(MatchingEngine& engine, Price bid) {
    engine.register_symbol("AAPL", 100.0);
    for (ClientId id = 1; id <= 4; ++id) engine.fund_balanced_account(id, 10000000.0, 0.5);
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 20000, 100.5));
    engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 20000, bid));
}

// steps the bot every 2 ms until it has seen `bars` bars (or 3 s pass)
void run_until(MLSignalBot& bot, std::uint64_t bars) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (bot.bars_seen() < bars && std::chrono::steady_clock::now() < deadline) {
        bot.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

TEST_CASE(ml_bots_share_the_same_bar_grid) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine, 99.5);
    auto model = always_up_model();
    InProcessGateway gateway_a(engine, 3), gateway_b(engine, 4);
    MLSignalBot a(gateway_a, {"AAPL"}, 1, {}, model, kBar);
    MLSignalBot b(gateway_b, {"AAPL"}, 2, {}, model, kBar);
    // two bots ticking at different moments over the same 11 bars see the same bars, without drift
    auto end = std::chrono::steady_clock::now() + 11 * kBar;
    while (std::chrono::steady_clock::now() < end) {
        a.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        b.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    CHECK(a.bars_seen() >= 10 && a.bars_seen() <= 13);
    CHECK(a.bars_seen() >= b.bars_seen() ? a.bars_seen() - b.bars_seen() <= 1 : b.bars_seen() - a.bars_seen() <= 1);
}

TEST_CASE(ml_bot_closes_its_position_after_the_model_horizon) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine, 99.5);
    InProcessGateway gateway(engine, 3);
    MLSignalBot bot(gateway, {"AAPL"}, 1, {}, always_up_model(), kBar);
    run_until(bot, kSignalMinHistory); // the history it needs before its first decision
    CHECK_EQ(bot.positions_opened(), std::uint64_t{1});
    CHECK_EQ(bot.open_positions(), std::size_t{1});
    run_until(bot, kSignalMinHistory + 2);
    CHECK_EQ(bot.positions_opened(), std::uint64_t{1}); // one position per symbol: no new entry while it is open
    CHECK_EQ(bot.horizon_exits(), std::uint64_t{0});
    run_until(bot, kSignalMinHistory + 3); // 3 bars after the entry: the horizon
    CHECK_EQ(bot.horizon_exits(), std::uint64_t{1});
    CHECK_EQ(bot.stop_exits(), std::uint64_t{0});
    CHECK(bot.positions_opened() <= 2); // closed, then (the signal still says up) at most one new position
}

TEST_CASE(ml_bot_closes_a_position_whose_stop_was_released_but_not_filled) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine, 90.0); // the only bid is far below
    InProcessGateway gateway(engine, 3);
    MLSignalBot bot(gateway, {"AAPL"}, 1, {}, always_up_model(), kBar);
    Quantity start = engine.portfolio_snapshot(3).holding("AAPL");
    run_until(bot, kSignalMinHistory);
    CHECK(engine.portfolio_snapshot(3).holding("AAPL") > start);
    // the price falls to 90: the stop (trigger 98) is released, becomes a sell limit at 98, and finds no buyer
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 5, 90.0));
    engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK(engine.portfolio_snapshot(3).holding("AAPL") > start); // still holding the position
    run_until(bot, kSignalMinHistory + 3);                         // the horizon
    CHECK_EQ(bot.horizon_exits(), std::uint64_t{1});              // the bot cancels the resting stop and sells itself
    CHECK_EQ(bot.stop_exits(), std::uint64_t{0});
    CHECK(engine.resting_orders("AAPL").size() <= 2);             // no leftover stop of the bot in the book
}

TEST_CASE(ml_bot_does_not_sell_twice_when_its_stop_filled) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    market(engine, 99.5);
    InProcessGateway gateway(engine, 3);
    MLSignalBot bot(gateway, {"AAPL"}, 1, {}, always_up_model(), kBar);
    Quantity start = engine.portfolio_snapshot(3).holding("AAPL");
    run_until(bot, kSignalMinHistory);
    CHECK_EQ(bot.positions_opened(), std::uint64_t{1});
    // the price falls to 96, then a buyer bids 98.2: the stop (trigger 98) is released and sells the whole position to it
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 20000, 99.5)); // takes the 99.5 bids
    engine.submit_order(nrt::make_order(4, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 96.0));
    engine.submit_order(nrt::make_order(1, Side::SELL, OrderKind::LIMIT, "AAPL", 10, 96.0));    // last price 96
    engine.submit_order(nrt::make_order(2, Side::BUY, OrderKind::LIMIT, "AAPL", 5000, 98.2));
    engine.try_release_waiting_orders(/*full_scan=*/true);
    CHECK_EQ(engine.portfolio_snapshot(3).holding("AAPL"), start); // the stop sold it all
    run_until(bot, kSignalMinHistory + 3);                         // the horizon
    CHECK_EQ(bot.stop_exits(), std::uint64_t{1});
    CHECK_EQ(bot.horizon_exits(), std::uint64_t{0});               // nothing left to close: no second sale
    CHECK(engine.portfolio_snapshot(3).holding("AAPL") >= start);
}
