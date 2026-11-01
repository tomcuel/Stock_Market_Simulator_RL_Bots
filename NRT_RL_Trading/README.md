# NRT_RL_Trading: non-regression tests

150 tests for `Src_RL_Trading`, all passing, also under AddressSanitizer. The exchange's tests are shared with
`Src_Simulation`'s suite ([NRT](../NRT/README.md)); `test_rl_bots.cpp` covers the C++ half of the bridge to the Python
agents: the policy client, the market feed and the RL bots. The Python half has its own check,
`python3 -m rl_agents.selftest` (see [Src_RL_Trading/README.md](../Src_RL_Trading/README.md#11-testing)). No external framework:
`nrt_framework.hpp` (about 70 lines) provides self-registering `TEST_CASE`, `CHECK`, `CHECK_EQ` and
`CHECK_NEAR`, so the suite builds anywhere a C++20 compiler exists.

## Running

```bash
cd NRT_RL_Trading
make test                  # builds nrt.x and runs it, non-zero exit status if anything fails
./nrt.x                    # run again without rebuilding
```

```
[PASS] noise_trader_bot_trades_across_multiple_symbols_not_just_one
[PASS] strategies_retract_their_stale_orders
...
[PASS] closing_auction_picks_the_price_executing_the_most_volume
[PASS] gtc_orders_survive_the_session_through_the_snapshot
...
[PASS] server_survives_clients_that_vanish_mid_reply
...
[PASS] rl_bot_goes_flat_then_short_trading_only_the_difference
[PASS] rl_bot_ignores_decisions_that_arrive_too_late
...
150 passed, 0 failed, 150 total
```

The engine logs at INFO level while the tests run, so the `[PASS]` lines come between log lines:
`./nrt.x | grep -E "PASS|FAIL|passed"` shows only the results.

Under AddressSanitizer (catches use-after-free and buffer overflows that the normal build can't
see):

```bash
SOURCES=$(find ../Src_RL_Trading/src -name "*.cpp" ! -name "main.cpp" ! -name "sim_server_main.cpp" ! -name "sim_client_main.cpp")
g++ -std=c++20 -O0 -g -fsanitize=address -I. -I../Src_RL_Trading/include -DNRT_OUTPUT_DIR="\"$(pwd)/output\"" \
    $SOURCES test_*.cpp runner.cpp -o nrt_asan.x -pthread && ./nrt_asan.x
```

The Makefile tracks header dependencies (`-MMD -MP`): changing a header rebuilds every test that
uses it. Tests that write files (reports, snapshots) write them under `NRT_RL_Trading/output/`,
never to `/tmp`: one numbered folder per test (`gtc_snapshot_1`, `persistence_roundtrip_10`...), so no test
reads or overwrites another one's files. Delete the folder whenever you like.

## What each file covers

| File | Tests | Covers |
|---|---|---|
| `test_order_book.cpp` | 15 | price-time priority, partial fills, market orders, cancels, concurrent submit and cancel on one book |
| `test_order_kinds.cpp` | 11 | STOP and LIMIT_STOP triggers, side-aware STOPs, start dates, expiries, waiting cancels |
| `test_rl_bots.cpp` | 11 | against a fake policy server written in C++ with scripted answers (no Python needed): the client (MODELS, INFO with the agent, lookback and every training seed listed in `train_seeds`, the exact daily OBSERVE line, ACT with the open trade), parsing answers and errors (a direction clamped to -1..+1, not-ready apart from failures), the MarketFeed pushing every symbol's daily bar. The RL bots (RLBot): an answer's stop, target and ATR parsed, the decision in the pre-open with a market-on-open entry sized on the stop distance (0.2% of the equity at risk, capped at 0.5% of it), then at the open a GTC STOP and a GTC LIMIT at the right prices and the open trade sent back (`dir= entry= sl= tp= days= tpr=`), one decision per day, the stop-loss filling and cancelling the target (one-cancels-other, about -1 R) with the real entry and exit fills, holding the next day then closing at the open when the agent goes flat, no decision outside the pre-open (counted as missed). A whole `Simulation` run with RL bots writing its `<agent> rl:` report lines and `rl_trades_simu.csv`, and an agent refused on the seed it trained on |
| `test_matching_engine.cpp` | 14 | validation, settlement, all portfolios, dirty-symbol and full release scans, the event bus, balanced starting accounts |
| `test_reservations.cpp` | 8 | open orders can't over-commit cash or shares, releases on cancel, expiry, partial fill and the discarded part of a MARKET order, the MARKET collar, and a 12-thread stress test ending with zero reserved and conserved cash |
| `test_concurrency.cpp` | 4 | cash and shares conserved under many threads, every engine lock under contention |
| `test_order_registry_concurrency.cpp` | 5 | the waiting registry under concurrent add, cancel and scan, exact accounting |
| `test_portfolio_metrics.cpp` | 7 | net worth, metrics, latency histogram, market data under concurrent writers and readers |
| `test_bots.cpp` | 11 | orders sized by value, every rule-based strategy: multi-symbol trading, stale order retraction, momentum threshold, market maker never crossing, mean reversion, trailing stop, TWAP completion, no strategy overdraws, the removed `ml` strategy refused by the factory |
| `test_market_recorder.cpp` | 7 | the market report: every file, before and after prices, cash conservation, unwritable folder, recorder lifetime, the rejections per client and the session phases |
| `test_net_protocol.cpp` | 7 | message framing: sizes, ordering, oversized header, disconnects, timeouts |
| `test_net_auth.cpp` | 12 | SHA-256 test vectors, password hashing, tokens, concurrent registration |
| `test_net_persistence.cpp` | 4 | snapshot save and load, precision, concurrent saves |
| `test_trading_days.cpp` | 9 | the trading days of `simulation.x`, its only mode: MARKET orders in an auction join at the 2% auction collar and expire after it (no cash reservation leaked), the market reopening with a GTC order's clock skipping the closed hours, overnight news moving the price the next day opens from, the daily bar (open, high, low, the official close, VWAP, gap, the book before the pre-close), the session's phases, a 3-day run writing and reporting its daily bars (and its value investors apart, as the informed benchmark, after the ranked strategies), a run asked for 0 days still trading one (no continuous mode), the session phases written in order, the fundamental values' variance split between night and day, a value investor buying a stock trading below its value |
| `test_closing_auction.cpp` | 12 | the trading day: pre-close orders rest without matching, the auction price (the most volume, then the smallest imbalance, then the closest to the last price), no cross keeps the last price, waiting orders held during pre-close, the market closed after the auction, DAY orders expire while GTC and dated orders stay, MARKET and expired orders never carried, the snapshot carrying orders (same ids, queue order, rebuilt reservations), the opening auction, an order's clock stopping at the close |
| `test_net_server_integration.cpp` | 13 | a real server on a real port: authentication, orders, bad commands, graceful stop, concurrent clients, clients vanishing mid-reply, cancel ownership, strategies over the WireGateway |

## Regression tests for real bugs

Several tests exist because a bug was found in a real run. Each one fails if the bug comes back:

| Test | Bug it pins down |
|---|---|
| `open_buy_orders_cannot_over_commit_cash` | open orders were never reserved: a bot ended a run at -18768.89 cash |
| `a_client_cannot_cancel_another_clients_orders` | any client could cancel any other client's orders by guessing their ids |
| `server_survives_clients_that_vanish_mid_reply` | one client disconnecting while the server replied killed the whole server (SIGPIPE). With the old code the whole suite dies with status 141 |
| `market_maker_quotes_never_cross_even_with_a_large_inventory` | an over-strong inventory lean made market makers cross the book and doubled the price swings |
| `momentum_bot_ignores_moves_smaller_than_its_threshold` | momentum bots chased their own price impact |
| `buy_stop_above_the_market_waits_instead_of_releasing_immediately` | BUY stops fired instantly and pushed every price up |
| `balanced_funding_gives_every_symbol_the_same_value` | 100 shares of every symbol made bots 98% stock with index prices: they could only sell, and every price drifted down |
| `a_closed_market_rejects_new_orders` | trading continued between the closing auction and the server's stop |
| `server_stops_at_once_even_with_a_long_save_interval` | a server stop waited for its snapshot thread's 30 s sleep to end (7.6 s in a real run) |
| (the whole suite under AddressSanitizer) | server connection threads were detached and could outlive the server: a use-after-return once stops became fast |
| `an_order_clock_stops_at_the_close` | orders' expiry clocks kept running while the market was closed, so dated orders looked expired at the next session |
| `market_orders_and_expired_orders_are_never_carried_over` | an unfilled MARKET order, given an expiry date, was carried to the next session |

## Adding a test

```cpp
#include "matching_engine.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"

using namespace sim;

TEST_CASE(my_new_behavior) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    engine.ensure_client(1, 100000.0);
    auto result = engine.submit_order(nrt::make_order(1, Side::BUY, OrderKind::LIMIT, "AAPL", 10, 99.0));
    CHECK(result.accepted);
    CHECK_NEAR(engine.portfolio_snapshot(1).reserved_cash, 990.0, 1e-9);
}
```

The checks: `CHECK(condition)`, `CHECK_EQ(a, b)`, `CHECK_NEAR(a, b, tolerance)`. The first failed check
stops that test and reports the expression (with both values for `CHECK_EQ`) and its line, then the
runner moves on to the next test.

Any `test_*.cpp` in this folder is picked up by the Makefile, nothing else to register. For a lock or
a shared state, follow `test_order_registry_concurrency.cpp`: many threads at once, then an exact
accounting check that only holds if the lock does its job.
