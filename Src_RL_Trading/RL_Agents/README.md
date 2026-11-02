# RL_Agents: the RL agent's code, file by file
The Python side of `Src_RL_Trading`: our own PPO (NumPy only) trained by walk-forward on trading days recorded in the simulated exchange, and the policy server the C++ simulator's RL bots ask for their trades.

This page explains **how the code works**. The results, the live runs and the market's calibration are in [Src_RL_Trading/README.md](../README.md) (sections 4 to 7).

**Contents:**
1. [Why reinforcement learning, in 5 minutes](#1-why-reinforcement-learning-in-5-minutes)
2. [The big picture](#2-the-big-picture)
3. [`daily.py`: the recorded trading days](#3-dailypy-the-recorded-trading-days)
4. [`env.py`: the environment](#4-envpy-the-environment)
5. [`ppo.py`: PPO in NumPy](#5-ppopy-ppo-in-numpy)
6. [`train.py`: walk-forward training, the gate, the sealed markets](#6-trainpy-walk-forward-training-the-gate-the-sealed-markets)
7. [Serving: `serve.py`, `policy.py`, `protocol.py`](#7-serving-servepy-policypy-protocolpy)
8. [The other files](#8-the-other-files)
9. [Commands](#9-commands)
10. [Layout](#10-layout)

---

## 1. Why reinforcement learning, in 5 minutes
### Prediction vs decision
The usual machine-learning approach to trading is **supervised:** predict tomorrow's return, then a hand-written rule turns the forecast into trades ("buy if the forecast > x"). That has two weaknesses:
- the model is judged on forecast error, not on money: a good forecast can still lose after costs, stops and position sizing
- every trading choice (when to enter, where to put the stop, when to get out, whether to stay in a trade) is left to the rule

**Reinforcement learning (RL)** learns the decision directly. An **agent** interacts with an **environment** in a loop:
```text
          observation s_t                 
   ┌──────────────────────────┐           
   │                          ▼           
 environment              agent (policy π)
   ▲                          │           
   └──────────────────────────┘           
      action a_t   →   reward r_t, next observation s_t+1
```
- **observation** `s_t`: what the agent sees (here: the day's market features and its open trade)
- **action** `a_t`: what it does (here: flat / long / short, a stop-loss, a take-profit)
- **reward** `r_t`: the result (here: the day's P&L in units of the risk taken)
- **policy** `π(a | s)`: the agent's strategy, a probability over actions for each observation (here: a neural network)

The goal is the policy that maximizes the **expected sum of discounted rewards** `r_t + γ r_t+1 + γ² r_t+2 + …`, with `γ < 1` (0.95 here) so that a reward matters less the further away it is. The agent is not told which action was right: it tries, sees what happens, and makes good actions more likely.

### Why it fits this problem
- **Trading is sequential:** what you do today (open a trade) changes what you can do tomorrow (you are now holding it). RL optimizes over that sequence, a forecast does not.
- **The objective is the P&L itself:** costs included: the spread, the slippage, the stop that fills past its price are all inside the environment, so the agent learns *with* them.
- **The whole bracket is learned:** direction, stop distance and reward-to-risk ratio are one action, not three separate rules.
- **We have a simulator:** RL needs millions of trials, which real markets cannot give (one history, and no way to try another action on the same day) The C++ exchange can record as many markets as we want, each with other news and other order flow, so the agent never trains and is judged on the same history.

### Why PPO
Two families of RL algorithms:
- **value-based** (Q-learning, DQN): learn the value of each action, act greedily. Unstable with noisy rewards, awkward with a combined action (3 × 3 × 4 choices)
- **policy-gradient** (REINFORCE, A2C, **PPO**): learn the policy directly, push up the probability of actions that did better than expected

**PPO** (Proximal Policy Optimization) is the standard policy-gradient method because it is **stable:**
- an **actor** (the policy) and a **critic** (a value function `V(s)` that estimates the future rewards from `s`)
- the **advantage** `A = what happened − what the critic expected` tells whether an action was better or worse than usual (estimated by GAE, which trades bias against variance with `λ`)
- the update makes good actions more likely, but the **clipped objective** forbids moving the policy far from the one that collected the data (the probability ratio stays within `1 ± 0.1`): no single noisy batch of trades can wreck the policy
- an **entropy bonus** keeps it exploring instead of locking on the first strategy that worked

It handles a factorized discrete action (one softmax per component) naturally, which is exactly our bracket.

### Why the protocol around it matters more than the algorithm
Markets are mostly noise, and an RL agent will happily **overfit:** find a strategy that worked on its training days by chance. So most of the code is not PPO but the protection against fooling ourselves:
- **walk-forward folds:** train on the past, choose on the next days, judge on the days after (never seen by any choice)
- **a checkpoint rule:** demands profit on two separate windows, not one
- **a gate:** refuses to ship an agent that is not consistent across folds
- **sealed markets:** evaluated once, at the end
- **baselines:** flat, always long, a tuned trend rule, random, buy-and-hold on the same windows, an agent is only good if it beats them

---

## 2. The big picture
```text
 C++ simulation.x --days                      (scripts/simulate_market_days.sh)
        │  one folder per seed = one market (same rules, other random draws)
        ▼
 sim_days/<market>/seed_<S>/daily_bars_simu.csv, trades_simu.csv
        │
        ▼
 daily.py  ── features, intraday price paths, the walk-forward calendar
        ▼
 env.py    ── RLEnv: one step = one trading day, a bracket trade
        ▼
 ppo.py    ── the PPO agent
        ▼
 train.py  ── folds → checkpoint rule → gate → sealed markets → baselines
        │      writes artifacts/<name>*, reports/<name>_*  ──► plots.py (charts)
        ▼
 serve.py + policy.py ── TCP policy server (port 7979), framing in protocol.py
        ▲
        │ OBSERVE at every close, ACT at every open
 C++ RLBot (src/rl_bot.cpp) trades the answer with real STOP and LIMIT orders
```
Two phases:
- **offline:** record markets, train, judge (`train.py`)
- **live:** the simulator runs RL bots that ask the server what to trade, **nothing is learned while serving**

---

## 3. `daily.py`: the recorded trading days
**Input:** A recorded market is a folder of runs, one per seed (`real5_d600`: 40 seeds × 5 symbols × 600 days):
- `daily_bars_simu.csv`: one row per symbol per day: open, high, low, close (the closing auction), volume, VWAP, the book before the pre-close, the overnight gap
- `trades_simu.csv`: every trade with its time

**`add_features(bars)`:** 23 market features, all **causal** (day `t` only uses days ≤ `t`) and **scale-free** (distances divided by the ATR, ratios), so the same agent works on any price level:
| Group | Features |
|---|---|
| trend | close − EMA5/10/20, EMA5 − EMA20 (÷ ATR) |
| momentum | MACD histogram, 10-day rate of change (÷ ATR) |
| volatility regime | ATR / close, fast ATR / slow ATR, Bollinger width |
| the day's candle | range, upper and lower wick, body |
| recent moves | 1 to 5-day returns (÷ ATR) |
| overnight news | the gap (÷ ATR) |
| activity | volume and trade-count z-scores |
| the book before the close | spread in bp, depth imbalance |

`WARMUP = 20`: the first 20 days are unusable (EMA20, ATR14).

**Guards:**
- `leakage_check`: the features of the first `cut` days must not change when later days are appended (no look-ahead)
- `feature_audit`: reports the near-duplicate pairs (|r| ≥ 0.96), never drops one

**`Stream`:** one symbol of one seed: the features as a plain array `x`, the `open/high/low/close/atr/spread_bps` arrays (pandas lookups were 10× slower in the environment's loop), and **`intraday[d]`**, the day's trade prices in order. That path is what decides whether the stop or the target was hit first. Without a trades file it is approximated by open → the extreme nearest the open → the other extreme → close.

**`sliding_folds`:** the walk-forward calendar on the day index, the same for every market:
```text
day 20      200   232   264
 │ train 180 │e│ val 30 │e│ test 30 │          fold 1   (e = 2-day embargo)
       │ train 180 │e│ val 30 │e│ test 30 │    fold 2   (slides by 30)
                         …                     12 folds on 600 days, 360 test days
```
`daily_statistics` / `market_statistics`: the market's daily statistics (drift, volatility, autocorrelation, range, gaps, spread, the book's size), used by `calibration.py` and `scripts/market_calibration.py`.

---

## 4. `env.py`: the environment
**One step = one trading day:** at the close of day `t` the agent sees:
- **the observation** (29 numbers): the 23 market features + 6 features of its open trade (direction, unrealized result in R, days held, distance to the target and to the stop in ATR, the trade's reward-to-risk ratio)
- **the action** (`MultiDiscrete [3, 3, 4]`, 36 combinations):
  - direction: 0 flat / close, 1 long, 2 short
  - stop-loss: `SL_ATR = (1.0, 1.5, 2.0)` × ATR from the entry
  - take-profit: `TP_R = (1, 1.5, 2, 3)` × the stop distance

**`step()` on day `t+1`**, what the C++ RL bot does live (its orders reach the next open):
1. a **close or a flip** is executed at the open, paying half the spread + `slippage_bps` (5 bp)
2. a **new trade** enters at the open, same costs. Its size risks `risk_fraction` (0.2%) of the equity on the stop distance, capped at `max_notional` (0.5% of the equity): the simulated book is thin
3. **`_walk_day`** walks the day's recorded trades in order:
   - the first one at or through the stop fills the stop at *that* price (a gap fills worse, like a real stop order), minus `stop_slippage_bps` (100 bp, measured live by `scripts/evaluate_rl_live.sh`)
   - the first one at or through the target fills at the target (a resting limit order)
   - stop and target are GTC: they carry to the next days
4. **reward** = the day's equity change ÷ the risk budget (in R, which keeps PPO stable) + `mtm_weight` (0.01) × the open trade's unrealized R − `holding_penalty` per day held

**Episodes:** in training, 40 days (`max_steps`) from a random start inside the window, on a random stream. `rollout()` runs a whole window from its start for evaluation, an open trade is marked to market at the end, never force-closed.

---

## 5. `ppo.py`: PPO in NumPy
| Piece | What it is |
|---|---|
| `MLP` | tanh hidden layers (128, 64), linear output, orthogonal init, `forward` keeps what `backward` needs |
| policy network | 29 → 10 logits, split into 3 softmax heads, the action's log-probability is the sum of the heads' |
| value network | 29 → 1, the critic |
| `RunningNorm` | running mean/variance of the observations (like VecNormalize) and of the returns (reward scaling). **Saved with the model:** serving uses the normalization it was trained with |
| `Adam` | weight decay, gradients clipped to a norm of 0.5 |
| `collect()` | runs the **stochastic** policy for `n_steps` (2048) transitions, new episode when one ends |
| `update()` | GAE advantages (γ, λ = 0.95), normalized per minibatch, clipped surrogate (0.1) + entropy bonus (0.03) + squared value loss, 5 epochs, early stop when the approximate KL passes 1.5 × 0.025, learning rate decaying linearly to 0 |
| `state()` / `from_state()` / `snapshot()` | save, reload, freeze a checkpoint |

The gradients are written by hand, `selftest.py` checks every one of them against finite differences.

---

## 6. `train.py`: walk-forward training, the gate, the sealed markets

`python -m rl_agents.train --data sim_days/real5_d600 --jobs 8`, in order:

1. **Markets:** The last `--holdout` seeds (8) are **sealed:** nothing ever sees them until the very end. The others are development markets.
2. **Trainings:** One per (fold, PPO seed): 12 folds × `--seeds`. `train_all` runs them in `--jobs` processes, every training is seeded, so the result is the same as serial.
3. **`train_fold`**, the **checkpoint rule**. PPO trains on 40-day episodes inside the fold's train window, on every development stream. Every `--eval-every` steps (20,480) the **greedy** policy is run on two legs:
   - *train-eval*: the last 30 days of the train window
   - *validation*: the validation window

   Each leg scores `return% − 0.5 × maxDD%`, **the checkpoint's score is the weaker leg**. Only a checkpoint profitable on both legs with at least `--min-trades` (10) trades on each is eligible: an idle policy, with no drawdown, must not win by doing nothing.
4. **Per fold**, the PPO seed with the best **validation** score is kept (never chosen on the test), then run on the **test** window (`book()`: each market is a portfolio of its symbols, equal capital).
5. **Baselines** on the same test window: `flat`, `always_long` (a fixed 1.5 ATR / 1.5 R bracket), `trend_hold` (long above the 20-day trend, short below, grid-tuned on train + validation only), `random`, buy-and-hold.
6. **`stitch`:** the 12 test windows compounded into one continuous out-of-sample curve per market.
7. **`gate()`:** the agents ship (`gate_passed=true`) only if
   - ≥ 75% of the folds have a test return > 0 and a profit factor > 1
   - the worst fold's profit factor is ≥ 0.9
   - the mean test Sharpe is > 0
8. **Sealed markets**, evaluated once on their whole history: the production model (the last fold's), the **ensemble** of the last `--ensemble` (3) folds (each trades its own book, equal capital: averaging their actions would make incoherent trades), and the baselines.
9. **Saved:**
   - `artifacts/<name>_f1 … _f12`: every fold model (`ppo.pkl` + `metadata.json`: features, brackets, the env, the gate, the provenance)
   - `artifacts/<name>`: production (the last fold), with the sealed results and the ensemble's members
   - `artifacts/<name>_always_long`, `<name>_trend_hold`: the baselines, served live next to the agent
   - `reports/<name>_folds.csv, _evals.csv, _oos_equity.csv, _oos_baselines.csv, _test_actions.csv, _test_trades.csv, _summary.json`
   - the charts: `reports/<name>/plots/index.html`

The logs show returns in **bp:** a trade risks 0.2% of the equity and is capped at 0.5% notional, so the agent's returns are small by construction, and are not comparable to buy-and-hold's full exposure.

---

## 7. Serving: `serve.py`, `policy.py`, `protocol.py`

**`protocol.py`:** the framing of `src/net/protocol.cpp`: an 8-byte big-endian length, then the text (at most 1 MiB).

**`serve.py`:** `python -m rl_agents.serve` (port 7979, one thread per connection):
- `load_policies`: every artifact whose `agent` is `ppo` or `rule`. An agent that **failed the gate is refused** unless `--allow-ungated`
- `StreamStore`: a rolling buffer of daily bars per symbol
- the commands:

| Command | Answer |
|---|---|
| `OBSERVE <symbol> <day> <close> <bid> <ask> <bid_depth> <ask_depth> <volume> <open> <high> <low> <trades>` | `OK OBSERVED symbol=.. n=..` (pushed by the C++ MarketFeed at every close) |
| `ACT <model> <symbol> dir=.. entry=.. sl=.. tp=.. days=.. tpr=..` | `OK ACTION model=.. symbol=.. position=.. direction=.. sl_atr=.. tp_r=.. atr=.. close=.. p_long=.. p_flat=.. p_short=..`, or `ERR NOT_ENOUGH_DATA` before the 21 days of warm-up |
| `MODELS` | the served agents |
| `INFO <model>` | its type, lookback, the seeds it trained on |
| `STATUS <model>` | observations, decisions, long / flat / short counts, latency |
| `QUIT` | closes the connection |

**`policy.py`:**
- `RLModel.observation` recomputes `add_features` on the symbol's recent bars and rebuilds the 6 position features from the bot's open trade, **exactly as the environment does** (an equity of 1, the trade's own stop distance as its risk: only ratios are used)
- `act` answers the **greedy** action (the most likely of each head) and the direction probabilities
- `RuleModel`: `always_long` and `trend_hold` served like an agent (never gated: they are baselines)
- `RLServed`: the decision cache per symbol (same bar count + same trade = same answer) and the STATUS counters

**The C++ side** ([src/rl_bot.cpp](../src/rl_bot.cpp)) sizes the trade with the same rule as the environment, `min(risk_fraction × equity / (sl_atr × atr), max_notional × equity / price)`, and places a GTC stop and a GTC target. [scripts/policy_server.sh](../scripts/policy_server.sh) starts and stops the server for the launchers and serves the ensemble members by default.

---

## 8. The other files

- **`calibration.py`:** is the recorded market a stock market? Its daily statistics, return and gap distributions and autocorrelation against the real tickers it starts from (`Data/Datasets/processed/all_prices.csv`), as charts in `reports/<market>/plots/`
- **`plots.py`:** every chart: the calibration, the training (learning curves, PPO diagnostics, the fold table, the out-of-sample equity, the actions, the sealed markets) and the live runs (equity, trades on candles, execution costs, activity, used by `scripts/plot_rl_report.py`)
- **`selftest.py`:** `python -m rl_agents.selftest` checks every module imports, no look-ahead, the 600-day calendar, parallel = serial training, the stops and targets, PPO's gradients against finite differences, that PPO learns to go long in a rising market, the same action after reload, and the server over a real socket
- **`paths.py`:** paths shown relative to the project root

---

## 9. Commands

From `Src_RL_Trading/RL_Agents/`:

```bash
python -m rl_agents.selftest
```

```bash
python -m rl_agents.calibration --data sim_days/real5_d600
```

```bash
python -m rl_agents.train --data sim_days/real5_d600 --jobs 8
```

```bash
python -m rl_agents.plots training --name rl
```

```bash
python -m rl_agents.serve --models rl_f10 rl_f11 rl_f12 --allow-ungated
```

The markets are recorded by `../scripts/simulate_market_days.sh`, the live runs launched by `../launch.sh` (see [Src_RL_Trading/README.md](../README.md#9-programs-and-scripts)).

---

## 10. Layout

```
RL_Agents/
├── rl_agents/
│   ├── daily.py         recorded days: features, streams, folds, market statistics
│   ├── env.py           the environment: one day per step, bracket trades
│   ├── ppo.py           PPO in NumPy
│   ├── train.py         walk-forward training, gate, ensemble, sealed markets, baselines
│   ├── policy.py        a trained agent (or a rule) as the server serves it
│   ├── serve.py         the policy server
│   ├── protocol.py      the framing shared with C++
│   ├── calibration.py   the market against the real tickers
│   ├── plots.py         every chart
│   ├── selftest.py      the checks
│   └── paths.py         display paths
├── sim_days/<market>/seed_<S>/   recorded markets (simulate_market_days.sh)
├── artifacts/<name>[_f<k>|_<rule>]/   trained agents: ppo.pkl + metadata.json
└── reports/              training CSVs, logs, summary, charts
```
