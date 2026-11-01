"""
A trained RL agent (train.py) as the policy server serves it to the C++ RLBot

At each open the bot asks
    ACT <model> <symbol> dir=<-1|0|1> entry=<price> sl=<price> tp=<price> days=<n> tpr=<R>
(its open trade on the symbol, dir=0 when flat). The server builds the agent's observation from the symbol's daily bars
(the stream the MarketFeed pushed at every close: open, high, low, close, volume, trades, the book before the pre-close)
plus that trade, exactly as the training environment does, and answers the greedy action:
    OK ACTION model=.. symbol=.. position=<-1|0|1> direction=<-1|0|1> sl_atr=1.5 tp_r=2 atr=<price units> close=..
              p_long=.. p_flat=.. p_short=..
The bot then sizes the trade on sl_atr x atr (risk_fraction of its equity, capped) and places the stop and the target.
Nothing is learned while serving: PPO learns offline from whole rollouts of its own stochastic policy (train.py).
"""
from __future__ import annotations

import json
import pickle
import threading
import time
from pathlib import Path

import numpy as np
import pandas as pd

from .env import Trade, position_features
from .daily import FEATURES, add_features
from .ppo import PPOAgent


class RLModel:
    type_name = "ppo"
    def __init__(self, agent: PPOAgent, meta: dict):
        self.agent, self.meta = agent, meta
        self.sl_atr, self.tp_r = list(meta["sl_atr"]), list(meta["tp_r"])
        self.lookback = int(meta.get("warmup", 20)) + 1
        self.name = meta.get("name", "")
        self.provenance = meta.get("provenance", {})
        self.gate_passed = bool(meta.get("gate_passed", False)) or meta.get("agent") == "rule" # a rule is a baseline, never gated

    @classmethod
    def load(cls, folder: Path) -> "RLModel":
        folder = Path(folder)
        meta = json.loads((folder / "metadata.json").read_text())
        with open(folder / "ppo.pkl", "rb") as f:
            agent = PPOAgent.from_state(pickle.load(f))
        return cls(agent, meta)

    @staticmethod
    def is_rl(folder: Path) -> bool:
        meta = Path(folder) / "metadata.json"
        return meta.exists() and json.loads(meta.read_text()).get("agent") in ("ppo", "rule")

    @staticmethod
    def load_any(folder: Path) -> "RLModel":
        meta = json.loads((Path(folder) / "metadata.json").read_text())
        return RuleModel(meta) if meta.get("agent") == "rule" else RLModel.load(folder)

    @staticmethod
    def bars(frame: pd.DataFrame) -> pd.DataFrame:  # the server's stream (one row per day) in the daily bars' column names
        return pd.DataFrame({"open": frame["open"], "high": frame["high"], "low": frame["low"], "close": frame["close"],
                             "volume": frame["volume"], "trades": frame["trades"].fillna(0.0), "bid": frame["bid"], "ask": frame["ask"],
                             "bid_depth": frame["bid_depth"], "ask_depth": frame["ask_depth"]}).reset_index(drop=True)

    def observation(self, frame: pd.DataFrame, state: dict) -> tuple[np.ndarray, float, float]:
        days = add_features(self.bars(frame))
        last = days.iloc[-1]
        x = np.nan_to_num(last[FEATURES].to_numpy(dtype=float), nan=0.0, posinf=10.0, neginf=-10.0)
        direction = int(state.get("dir", 0))
        trade = Trade()
        if direction != 0:
            entry, sl = float(state.get("entry", 0.0)), float(state.get("sl", 0.0))
            sl_dist = max(abs(entry - sl), 1e-12)
            # the observation only uses ratios: an equity of 1 and the trade's own stop distance as its risk
            trade = Trade(direction, entry, sl, float(state.get("tp", 0.0)), 1.0 / sl_dist, 1.0, 0.0, float(state.get("tpr", 0.0)), int(state.get("days", 0)), 0)
        pos = position_features(trade, float(last["close"]), float(last["atr"]), 1.0)
        return np.concatenate([x, pos]), float(last["atr"]), float(last["close"])

    def act(self, frame: pd.DataFrame, state: dict) -> dict:
        obs, atr, close = self.observation(frame, state)
        action, _, _ = self.agent.act(obs, deterministic=True)
        probs = self.agent.probabilities(obs)[0]
        direction = {0: 0, 1: 1, 2: -1}[action[0]]
        return {"position": direction, "direction": direction, "sl_atr": self.sl_atr[action[1]], "tp_r": self.tp_r[action[2]], "atr": atr, "close": close, "p_flat": float(probs[0]), "p_long": float(probs[1]), "p_short": float(probs[2])}


class RuleModel(RLModel):
    """
    A fixed rule served like an agent, the baselines judged live next to it (train.py saves them):
    always_long (a fixed bracket) and trend_hold (long above the 20-day trend, short below, tuned on train/validation)
    """
    type_name = "rule"
    def __init__(self, meta: dict):
        super().__init__(None, meta)
        self.rule = meta["rule"]
        self.params = meta.get("params", {})

    @classmethod
    def load(cls, folder: Path) -> "RuleModel":
        return cls(json.loads((Path(folder) / "metadata.json").read_text()))

    def act(self, frame: pd.DataFrame, state: dict) -> dict:
        obs, atr, close = self.observation(frame, state)
        if self.rule == "always_long":
            action = [1, int(self.params.get("sl_k", 1)), int(self.params.get("tp_k", 1))]
        else:
            from .train import trend_hold_factory
            class Held: # the rule only reads the open trade's direction
                class trade:
                    direction = int(state.get("dir", 0))
            action = trend_hold_factory(**self.params)(obs, Held)
        direction = {0: 0, 1: 1, 2: -1}[action[0]]
        return {"position": direction, "direction": direction, "sl_atr": self.sl_atr[action[1]], "tp_r": self.tp_r[action[2]], "atr": atr, "close": close}


class RLServed:
    """
    The server's bookkeeping around an RLModel: the decision cache per symbol and the STATUS counters
    """
    def __init__(self, name: str, model: RLModel, store, log=print):
        self.name, self.model, self.store, self.log = name, model, store, log
        self.lock = threading.Lock()
        self.cache: dict[str, tuple] = {}
        self.stats = {"observations": 0, "decisions": 0, "long": 0, "flat": 0, "short": 0, "latency_ms": 0.0}

    def act(self, symbol: str, state: dict | None = None) -> dict:
        state = state or {}
        count = self.store.count(symbol)
        key = (count, tuple(sorted(state.items())))
        cached = self.cache.get(symbol)
        if cached is not None and cached[0] == key:
            return cached[1]
        started = time.perf_counter()
        frame = self.store.frame(symbol, self.model.lookback + 64)
        with self.lock:
            out = self.model.act(frame, state)
        elapsed = (time.perf_counter() - started) * 1000
        self.stats["latency_ms"] += (elapsed - self.stats["latency_ms"]) / min(self.stats["decisions"] + 1, 100)
        self.stats["decisions"] += 1
        self.stats[{1: "long", 0: "flat", -1: "short"}[out["direction"]]] += 1
        self.cache[symbol] = (key, out)
        return out

    def on_observe(self, symbol: str) -> None:
        self.stats["observations"] += 1
