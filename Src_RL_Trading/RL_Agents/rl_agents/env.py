"""
The RL environment on recorded trading days (daily.py), the XAUUSD reference project's design on the simulated exchange: one step = one trading day

At the close of day t the agent sees the day's features and its open trade, and chooses (MultiDiscrete):
    direction   0 flat / close, 1 long, 2 short
    stop-loss   SL_ATR[k] x ATR away from the entry
    take-profit TP_R[k] x the stop distance away (the reward-to-risk ratio of the trade)
On day t+1 (what the C++ RLBot does live: its orders reach the market at the next open):
    - a close or a flip is executed at the open, paying half the spread (and the slippage)
    - a new trade enters at the open (same costs); its size risks risk_fraction (0.2%) of the equity on the stop distance,
      capped at max_notional (0.5%) of the equity: the simulated book is thin (about 8 million traded per symbol per day,
      165 shares a trade), and live, at 3% (about 1000 shares), a released stop walked it 1 to 2% past the stop
    - while it is open, the day's recorded trades are walked in order: the first one at or through the stop fills the
      stop-loss at that price (a gap through the stop fills worse than the stop, like a stop order) minus the stop slippage
      measured live (stop_slippage_bps, 100 bp), the first one at or
      through the take-profit fills it at the take-profit (a resting limit order). 
      Stop and target are GTC: they carry to the next days until one of them fills or the agent closes the trade
Reward (in units of the risk taken, which keeps PPO stable): the equity change over the day / the risk budget, plus mtm_weight x the open trade's unrealized result in R, minus holding_penalty per day held
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .daily import FEATURES, Stream

SL_ATR = (1.0, 1.5, 2.0)
TP_R = (1.0, 1.5, 2.0, 3.0)
HEADS = [3, len(SL_ATR), len(TP_R)]
N_POSITION_FEATURES = 6
DEFAULTS = {"risk_fraction": 0.002, "max_notional": 0.005, "slippage_bps": 5.0, "commission": 0.0,
            "holding_penalty": 0.00002, "mtm_weight": 0.01, "max_steps": 40,
            # what a stop-loss costs past the stop, measured live (scripts/evaluate_rl_live.sh): the C++ bot's 1% stop-limit filled a median 97 bp past the stop
            # The recorded trades alone said a few bp: they don't hold the cascade of every stop an overnight gap releases at the open, nor the bot's own sale in it
            "stop_slippage_bps": 100.0}


def obs_dim() -> int:
    return len(FEATURES) + N_POSITION_FEATURES


@dataclass
class Trade:
    direction: int = 0
    entry: float = 0.0
    sl: float = 0.0
    tp: float = 0.0
    units: float = 0.0      # in equity units per price unit (notional / price)
    risk: float = 0.0       # the cash it risks on the stop
    sl_atr: float = 0.0
    tp_r: float = 0.0
    days: int = 0
    entry_day: int = 0


def market_features(stream: Stream, day: int) -> np.ndarray:
    return stream.x[day]


def position_features(trade: Trade, close: float, atr: float, equity: float) -> np.ndarray:
    if trade.direction == 0:
        return np.zeros(N_POSITION_FEATURES)
    atr = max(atr, 1e-12)
    unrealized = (close - trade.entry) * trade.units * trade.direction
    return np.array([trade.direction, unrealized / max(trade.risk, 1e-12), min(trade.days / 10.0, 10.0), (trade.tp - close) * trade.direction / atr, (close - trade.sl) * trade.direction / atr, trade.tp_r])


class RLEnv:
    """
    One episode = one symbol of one market over a window of execution days [lo, hi)
    """
    def __init__(self, streams: list[Stream], window: tuple[int, int], config: dict | None = None, rng=None, random_start: bool = True, stream_index: int | None = None):
        self.streams, self.window = streams, window
        self.cfg = {**DEFAULTS, **(config or {})}
        self.rng = rng if rng is not None else np.random.default_rng()
        self.random_start, self.stream_index = random_start, stream_index

    # ---- episode
    def reset(self) -> np.ndarray:
        lo, hi = self.window
        k = self.stream_index if self.stream_index is not None else int(self.rng.integers(len(self.streams)))
        self.stream = self.streams[k]
        hi = min(hi, self.stream.n_days)
        self.hi = hi
        max_steps = int(self.cfg["max_steps"]) if self.random_start else hi - lo
        if self.random_start and hi - lo > max_steps:
            start = int(self.rng.integers(lo, hi - max_steps + 1))
        else:
            start = lo
        self.end = min(hi, start + max_steps)
        self.day = start - 1          # the decision day: the close the agent just saw
        self.equity = 1.0
        self.trade = Trade()
        self.trades: list[dict] = []
        self.curve: list[float] = []
        return self.observation()

    def observation(self) -> np.ndarray:
        d = self.day
        return np.concatenate([market_features(self.stream, d), position_features(self.trade, self.stream.close[d], self.stream.atr[d], self.equity)])

    # ---- execution helpers
    def _half_cost(self, day: int) -> float:  # half the spread seen at the previous close plus the slippage, as a fraction of the price
        spread = float(self.stream.spread_bps[max(day - 1, 0)])
        return (0.5 * spread + self.cfg["slippage_bps"]) / 1e4

    def _close(self, price: float, day: int, reason: str) -> float:
        t = self.trade
        pnl = (price - t.entry) * t.units * t.direction - self.cfg["commission"]
        self.equity += pnl
        self.trades.append({"stream": f"{self.stream.run}/{self.stream.symbol}", "entry_day": t.entry_day, "exit_day": day,
                            "direction": t.direction, "entry": t.entry, "exit": price, "sl": t.sl, "tp": t.tp, "sl_atr": t.sl_atr,
                            "tp_r": t.tp_r, "pnl": pnl, "r": pnl / max(t.risk, 1e-12), "days": t.days, "reason": reason})
        self.trade = Trade()
        return pnl

    def _open(self, direction: int, sl_k: int, tp_k: int, day: int) -> None:
        atr = float(self.stream.atr[day - 1])
        open_price = float(self.stream.open[day])
        entry = open_price * (1 + direction * self._half_cost(day))
        sl_dist = max(SL_ATR[sl_k] * atr, 1e-8)
        risk = self.equity * self.cfg["risk_fraction"]
        units = min(risk / sl_dist, self.cfg["max_notional"] * self.equity / entry)
        self.trade = Trade(direction, entry, entry - direction * sl_dist, entry + direction * TP_R[tp_k] * sl_dist, units, units * sl_dist, SL_ATR[sl_k], TP_R[tp_k], 0, day)

    def _walk_day(self, day: int) -> None:
        """
        The day's trades in order: the stop or the target, whichever comes first
        """
        t = self.trade
        if t.direction == 0:
            return
        slip = self.cfg["stop_slippage_bps"] / 1e4
        for price in self.stream.intraday[day]:
            if t.direction == 1:
                if price <= t.sl:
                    self._close(min(price, t.sl) * (1 - slip), day, "SL")
                    return
                if price >= t.tp:
                    self._close(t.tp, day, "TP")
                    return
            else:
                if price >= t.sl:
                    self._close(max(price, t.sl) * (1 + slip), day, "SL")
                    return
                if price <= t.tp:
                    self._close(t.tp, day, "TP")
                    return

    # ---- step
    def step(self, action):
        direction = {0: 0, 1: 1, 2: -1}[int(action[0])]
        sl_k, tp_k = int(action[1]), int(action[2])
        day = self.day + 1 # the day the orders reach the market
        before = self.equity
        risk_unit = max(before * self.cfg["risk_fraction"], 1e-12)
        open_price = float(self.stream.open[day])
        if self.trade.direction != 0 and direction != self.trade.direction:
            self._close(open_price * (1 - self.trade.direction * self._half_cost(day)), day, "close" if direction == 0 else "flip")
        if self.trade.direction == 0 and direction != 0:
            self._open(direction, sl_k, tp_k, day)
        self._walk_day(day)
        reward = (self.equity - before) / risk_unit
        if self.trade.direction != 0:
            self.trade.days += 1
            unrealized = (float(self.stream.close[day]) - self.trade.entry) * self.trade.units * self.trade.direction
            reward += self.cfg["mtm_weight"] * unrealized / max(self.trade.risk, 1e-12) - self.cfg["holding_penalty"]
        else:
            unrealized = 0.0
        self.curve.append(self.equity + unrealized)
        self.day = day
        done = self.day + 1 >= self.end
        if done and self.trade.direction != 0 and not self.random_start:
            pass # evaluation: the open trade is marked to market in the curve, never force-closed (it would add a cost)
        return self.observation(), float(reward), done, {"equity": self.equity}


def rollout(policy, stream: Stream, window: tuple[int, int], config: dict | None = None):
    """
    Runs a policy (obs -> action) over a whole window of one stream
    Returns (marked equity curve, trades)
    """
    env = RLEnv([stream], window, config, random_start=False, stream_index=0)
    obs = env.reset()
    if env.end - (env.day + 1) <= 0:
        return np.array([1.0]), []
    done = False
    while not done:
        obs, _, done, _ = env.step(policy(obs, env))
    return np.array(env.curve), env.trades
