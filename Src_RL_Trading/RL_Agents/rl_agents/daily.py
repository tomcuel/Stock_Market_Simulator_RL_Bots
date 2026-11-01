"""
Trading days recorded by simulation.x --days: the daily bars, the intraday trades, the RL agent's features and the walk-forward folds

A recorded market (scripts/simulate_market_days.sh) is a folder of runs, one per seed:
    sim_days/<market>/seed_<S>/daily_bars_simu.csv   one row per symbol per day: open, high, low, close (the closing auction), volume, VWAP, the book before the pre-close, the overnight gap
    sim_days/<market>/seed_<S>/trades_simu.csv       every trade with its time: what happened inside each day, in order
Every seed is its own market: same rules and bots, other random draws (other news, other order flow).

Features (FEATURES, all causal, all scale-free: 
- distances and ranges divided by the ATR, ratios), 
- the daily translation of the reference XAUUSD project's 25, with lookbacks that fit a 60-day market (at most 20 days, WARMUP = 20)
Every one of them is kept, none is selected away during tuning; the correlation audit (feature_audit) only reports the pairs that are near duplicates
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

WARMUP = 20           # days of history a feature needs (EMA 20, ATR 14...)
ATR_DAYS = 14

FEATURES = [
    "close_ema5_atr", "close_ema10_atr", "close_ema20_atr", "ema5_ema20_atr",  # trend / distance to the means
    "macd_hist_atr", "roc10_atr",                                               # momentum
    "atr_close", "atr_fast_slow", "bb_width_close",                             # volatility regime
    "range_atr", "upper_wick_ratio", "lower_wick_ratio", "body_atr",            # the day's candle
    "ret1_atr", "ret2_atr", "ret3_atr", "ret4_atr", "ret5_atr",                 # recent moves
    "gap_atr",                                                                  # overnight news
    "volume_z", "trades_z",                                                     # activity
    "spread_bps", "depth_imbalance",                                            # the book before the close
]


def atr(bars: pd.DataFrame, n: int = ATR_DAYS) -> pd.Series:
    prev = bars["close"].shift(1)
    tr = pd.concat([bars["high"] - bars["low"], (bars["high"] - prev).abs(), (bars["low"] - prev).abs()], axis=1).max(axis=1)
    return tr.ewm(alpha=1 / n, adjust=False, min_periods=n).mean()


def add_features(bars: pd.DataFrame) -> pd.DataFrame:
    """
    bars: one symbol's days in order (open, high, low, close, volume, trades, bid, ask, bid_depth, ask_depth)
    Every feature at day t only uses days <= t (leakage_check verifies it)
    """
    out = bars.copy()
    c, eps = out["close"], 1e-12
    out["atr"] = atr(out)
    a = out["atr"].replace(0, np.nan)
    ema = {n: c.ewm(span=n, adjust=False, min_periods=n).mean() for n in (5, 10, 20)}
    out["ema20"] = ema[20]
    out["close_ema5_atr"] = (c - ema[5]) / (a + eps)
    out["close_ema10_atr"] = (c - ema[10]) / (a + eps)
    out["close_ema20_atr"] = (c - ema[20]) / (a + eps)
    out["ema5_ema20_atr"] = (ema[5] - ema[20]) / (a + eps)
    macd = c.ewm(span=6, adjust=False, min_periods=6).mean() - c.ewm(span=13, adjust=False, min_periods=13).mean()
    out["macd_hist_atr"] = (macd - macd.ewm(span=5, adjust=False, min_periods=5).mean()) / (a + eps)
    out["roc10_atr"] = (c - c.shift(10)) / (a + eps)
    out["atr_close"] = out["atr"] / c
    out["atr_fast_slow"] = out["atr"].ewm(span=3, adjust=False, min_periods=3).mean() / out["atr"].ewm(span=10, adjust=False, min_periods=10).mean()
    mid, sd = c.rolling(10, min_periods=10).mean(), c.rolling(10, min_periods=10).std()
    out["bb_width_close"] = 4 * sd / c
    rng = (out["high"] - out["low"]).replace(0, np.nan)
    out["range_atr"] = (out["high"] - out["low"]) / (a + eps)
    out["upper_wick_ratio"] = ((out["high"] - out[["open", "close"]].max(axis=1)) / rng).fillna(0.0)
    out["lower_wick_ratio"] = ((out[["open", "close"]].min(axis=1) - out["low"]) / rng).fillna(0.0)
    out["body_atr"] = (c - out["open"]) / (a + eps) # with overnight gaps the body is not the 1-day return (the XAUUSD project dropped it: no gaps there)
    for k in range(1, 6):
        out[f"ret{k}_atr"] = (c - c.shift(k)) / (a + eps)
    out["gap_atr"] = (out["open"] - c.shift(1)) / (a + eps)
    for col, name in (("volume", "volume_z"), ("trades", "trades_z")):
        lv = np.log1p(out[col].astype(float))
        out[name] = (lv - lv.rolling(10, min_periods=5).mean()) / (lv.rolling(10, min_periods=5).std() + 1e-6)
    book = (out["bid"] > 0) & (out["ask"] > 0)
    out["spread_bps"] = np.where(book, 1e4 * (out["ask"] - out["bid"]) / ((out["ask"] + out["bid"]) / 2), np.nan)
    # a day without a two-sided book keeps the last known spread (the median of the whole series would look ahead)
    out["spread_bps"] = out["spread_bps"].ffill().fillna(0.0)
    depth = out["bid_depth"] + out["ask_depth"]
    out["depth_imbalance"] = np.where(depth > 0, (out["bid_depth"] - out["ask_depth"]) / depth.where(depth > 0, 1.0), 0.0)
    out[FEATURES] = out[FEATURES].replace([np.inf, -np.inf], np.nan)
    return out


def leakage_check(bars: pd.DataFrame, cut: int, atol: float = 1e-9) -> float:
    """
    The features of the first `cut` days must not change when later days are appended (the XAUUSD project's guard)
    Returns the largest change (raises if above atol)
    """
    past = add_features(bars.iloc[:cut])[FEATURES]
    full = add_features(bars)[FEATURES].iloc[:cut]
    diff = float(np.nanmax(np.abs(past.to_numpy() - full.to_numpy()))) if cut > 0 else 0.0
    if diff > atol:
        raise AssertionError(f"look-ahead: a feature moved by {diff} when future days were appended")
    return diff


def feature_audit(frame: pd.DataFrame, threshold: float = 0.96) -> list[tuple[str, str, float]]:
    """
    Pairs of features with |correlation| >= threshold: reported, not dropped
    """
    corr = frame[FEATURES].corr().abs()
    pairs = []
    for i, a in enumerate(FEATURES):
        for b in FEATURES[i + 1:]:
            if corr.loc[a, b] >= threshold:
                pairs.append((a, b, float(corr.loc[a, b])))
    return pairs


# ---------------------------------------------------------------- the market's calibration (README section 2)
CALIBRATION = ["drift_bp", "vol_bp", "autocorr", "range_bp", "abs_gap_bp", "overnight_share", "spread_bp", "trades_per_day", "shares_per_trade", "value_per_trade_k", "traded_value_m"]
# what has to hold against the baseline market for a recording to be the same market: within +-15%
CALIBRATION_CHECKED = ["vol_bp", "range_bp", "abs_gap_bp", "spread_bp"]


def daily_statistics(bars: pd.DataFrame) -> dict:
    """
    One market's daily statistics, the median over its symbols: 
    drift, volatility and lag-1 autocorrelation of the close-to-close log returns, the day's range, the overnight gap (its mean absolute size and its share of the daily variance), the spread before the close, and the book's size (trades, shares per trade, value traded per day) 
    Plus the day clock: the median and longest time between two opens (day + night)
    The RL bots size their trades by value (a share of their equity), so the book's size that matters to them is value_per_trade_k: shares per trade move with the price
    """
    rows = []
    for _, g in bars.groupby("symbol"):
        g = g.sort_values("day")
        r = np.log(g["close"]).diff().dropna()
        gap = g["gap"].iloc[1:]
        quoted = (g["bid"] > 0) & (g["ask"] > 0)
        mid = (g["bid"] + g["ask"]) / 2
        trades = g["trades"].where(g["trades"] > 0)
        rows.append({"drift_bp": 1e4 * r.mean(), "vol_bp": 1e4 * r.std(), "autocorr": r.autocorr(1) if len(r) > 2 else np.nan,
                     "range_bp": 1e4 * ((g["high"] - g["low"]) / g["close"]).mean(), "abs_gap_bp": 1e4 * gap.abs().mean(),
                     "overnight_share": gap.var() / r.var() if r.var() > 0 else np.nan,
                     "spread_bp": 1e4 * ((g["ask"] - g["bid"]) / mid)[quoted].mean(),
                     "trades_per_day": g["trades"].mean(), "shares_per_trade": (g["volume"] / trades).mean(),
                     "value_per_trade_k": (g["volume"] * g["vwap"] / trades).mean() / 1e3,
                     "traded_value_m": (g["volume"] * g["vwap"]).mean() / 1e6})
    out = pd.DataFrame(rows, columns=CALIBRATION).median().to_dict()
    opens = bars.drop_duplicates("day").sort_values("day")["open_ms"].diff()
    out["day_clock_ms"], out["day_clock_max_ms"] = float(opens.median()), float(opens.max())
    return out


def market_statistics(path: Path, part: tuple[float, float] = (0.0, 1.0)) -> pd.DataFrame:
    """
    daily_statistics for every run of a recorded market, one row per seed, on the share `part` of its days (e.g. (2/3, 1) for the last third: does a long market stay the same market?)
    """
    rows = {}
    for folder in sorted((p for p in Path(path).iterdir() if (p / "daily_bars_simu.csv").exists()), key=run_seed):
        bars = pd.read_csv(folder / "daily_bars_simu.csv")
        n = bars["day"].max() + 1
        lo, hi = int(round(part[0] * n)), int(round(part[1] * n))
        rows[run_seed(folder)] = daily_statistics(bars[(bars["day"] >= lo) & (bars["day"] < hi)])
    return pd.DataFrame.from_dict(rows, orient="index")


@dataclass
class Stream:
    """
    One symbol of one run: its days with features, and the trades inside each day (prices in time order)
    """
    run: str
    seed: int
    symbol: str
    days: pd.DataFrame                 # indexed 0..n-1 by day, with FEATURES, atr, open/high/low/close, bid/ask
    intraday: list[np.ndarray]         # intraday[d] = trade prices of day d in order (the opening auction first)

    def __post_init__(self):
        # plain arrays for the environment's hot loop (pandas lookups were 10 times slower)
        self.x = np.nan_to_num(self.days[FEATURES].to_numpy(dtype=float), nan=0.0, posinf=10.0, neginf=-10.0)
        for col in ("open", "high", "low", "close", "atr", "spread_bps"):
            setattr(self, col, self.days[col].to_numpy(dtype=float))

    @property
    def n_days(self) -> int:
        return len(self.days)


def run_seed(folder: Path) -> int:
    name = Path(folder).name
    return int(name.split("_")[-1]) if name.startswith("seed_") and name.split("_")[-1].isdigit() else -1


def load_run(folder: Path, suffix: str = "_simu") -> list[Stream]:
    folder = Path(folder)
    bars = pd.read_csv(folder / f"daily_bars{suffix}.csv")
    trades_path = folder / f"trades{suffix}.csv"
    trades = pd.read_csv(trades_path, usecols=["elapsed_ms", "symbol", "price"]) if trades_path.exists() else None
    streams = []
    for symbol, g in bars.groupby("symbol"):
        g = g.sort_values("day").reset_index(drop=True)
        days = add_features(g)
        intraday = []
        if trades is not None:
            t = trades[trades["symbol"] == symbol].sort_values("elapsed_ms", kind="stable")
            times, prices = t["elapsed_ms"].to_numpy(), t["price"].to_numpy()
            for o, cl in zip(g["open_ms"].to_numpy(), g["close_ms"].to_numpy()):
                lo, hi = np.searchsorted(times, o - 1e-6, "left"), np.searchsorted(times, cl + 1e-6, "right")
                intraday.append(prices[lo:hi])
        else: # no trades recorded: the day's path is approximated by open, the extreme nearest the open, the other one, close
            for row in g.itertuples(index=False):
                first, second = (row.low, row.high) if abs(row.open - row.low) < abs(row.open - row.high) else (row.high, row.low)
                intraday.append(np.array([row.open, first, second, row.close]))
        streams.append(Stream(folder.name, run_seed(folder), str(symbol), days, intraday))
    return streams


def load_market(path: Path, seeds: list[int] | None = None) -> list[Stream]:
    path = Path(path)
    folders = sorted([p for p in path.iterdir() if (p / "daily_bars_simu.csv").exists()], key=run_seed) if path.is_dir() else []
    if (path / "daily_bars_simu.csv").exists():
        folders = [path]
    if not folders:
        raise SystemExit(f"no trading-day runs in {path}: record some first (scripts/simulate_market_days.sh)")
    streams = []
    for folder in folders:
        if seeds is None or run_seed(folder) in seeds:
            streams.extend(load_run(folder))
    return streams


@dataclass
class Fold:
    """
    Sliding walk-forward on the day index, the same calendar for every development market (seed):
    train on [train_start, train_end), pick the checkpoint on [val_start, val_end), judge on [test_start, test_end)
    `embargo` days are skipped at each boundary
    """
    k: int
    train: tuple[int, int]
    val: tuple[int, int]
    test: tuple[int, int]


def sliding_folds(n_days: int, train_days: int, val_days: int, test_days: int, step_days: int, embargo: int = 2, start: int = WARMUP) -> list[Fold]:
    folds, k, s = [], 1, start
    while True:
        train = (s, s + train_days)
        val = (train[1] + embargo, train[1] + embargo + val_days)
        test = (val[1] + embargo, val[1] + embargo + test_days)
        if test[1] > n_days:
            break
        folds.append(Fold(k, train, val, test))
        k += 1
        s += step_days
    return folds
