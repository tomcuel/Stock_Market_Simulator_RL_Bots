#!/usr/bin/env python3
"""
train_signal_model.py: trains the model the ML bot (MLSignalBot, include/bot.hpp) uses to trade

Python trains, C++ infers: this script learns a logistic regression answering "will the price be higher `horizon` bars from now?" from five price features, and writes it to a small CSV
The C++ bot loads that CSV once and evaluates the model itself (five features and a dot product, microseconds), so no Python process runs during the simulation

The five features are computed exactly like compute_signal_features() in src/signal_model.cpp (NRT/test_bots.cpp checks both sides agree):
    ret_1      p_t / p_{t-1} - 1
    ret_5      p_t / p_{t-5} - 1
    ma_gap_10  p_t / mean(p_{t-9} .. p_t) - 1
    vol_10     population standard deviation of the last 10 one-step returns
    rsi_14     (RSI - 50) / 50, RSI from the plain average gain / loss of the last 14 changes

Training data: the real market fetched by the Data/ pipeline (refresh_data.sh), by default
    Data/Datasets/processed/all_prices.csv     daily closes of every fetched ticker (features.csv works too)
    a Src_Simulation run folder (--input)      prices a simulation produced, cut into bars of --bar-ms
No previous simulation run is needed: refresh_data.sh trains the model right after fetching, and the launchers train it when ML bots are asked for and no model exists yet
Each symbol's series is split in time: the first --train-fraction trains the model, the rest tests it, so the test score is never computed on data the model has seen
The script prints the test accuracy next to the naive baseline (always predicting the most frequent answer)

Daily training, simulated inference: the model learns from one close per trading DAY, while a simulation moves tick by tick
The ML bot therefore reads the market in bars (--ml-bar-ms in simulation.x, 1000 ms by default): one bar of simulated time plays one trading day, it takes one close per bar and decides once per bar, so "5 bars ahead" means what "5 days ahead" meant in training
--check-run compares the features of the training data with those of a simulation run cut into bars, and suggests the bar length whose price moves match a real trading day (returns grow with the square root of time, so a bar twice as volatile as a day should be about 4 times shorter)

Usage (from Src_Simulation/):
    python3 scripts/train_signal_model.py                                          # default input: the fetched real closes
    python3 scripts/train_signal_model.py --input ../Data/Datasets/processed/features.csv --horizon 10
    python3 scripts/train_signal_model.py --input output/my_run --bar-ms 1000      # from a simulation run instead
    python3 scripts/train_signal_model.py --check-run output/latest --bar-ms 1000  # training data vs that run's bars
    ./simulation.x --ml 4 --ml-bar-ms 1000                                         # the ML bots trade with the model

Options (all optional):
    --input PATH           CSV of closes (ticker, date, close or Close) or a simulation run folder (default: Data/Datasets/processed/all_prices.csv)
    --suffix simu|bots     which price_samples file to read from a run folder (default simu)
    --bar-ms N             run folders: one close every N ms of simulated time (default 1000, the ML bot's default bar, 0 = every sample)
    --output PATH          model file (default: models/signal_model.csv)
    --horizon N            how many bars (days) ahead the label looks (default 5)
    --train-fraction F     share of each series used for training (default 0.7)
    --l2 X                 L2 regularization strength (default 0.01)
    --epochs N             gradient descent iterations (default 2000)
    --check-run RUN        compare the training features with the bars of this simulation run, no training
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

FEATURES = ["ret_1", "ret_5", "ma_gap_10", "vol_10", "rsi_14"]  # same order as kSignalFeatureNames in C++
MIN_HISTORY = 15                                                # kSignalMinHistory
SIM_DIR = Path(__file__).resolve().parent.parent
DATA_PROCESSED = SIM_DIR.parent / "Data" / "Datasets" / "processed"


def display_path(path) -> str:
    """
    A path as shown to the user: relative to the project root (the folder holding Src_Simulation/ and NRT/)
    e.g. Src_Simulation/models/signal_model.csv, never absolute
    """
    resolved = Path(path).resolve()
    for parent in [resolved, *resolved.parents]:
        if (parent / "Src_Simulation").is_dir() and (parent / "NRT").is_dir():
            return str(resolved.relative_to(parent))
    return str(path)


def features_at(p: np.ndarray, t: int) -> list[float]:
    """
    The five features of p[t], mirroring compute_signal_features() line by line
    """
    ret_1 = p[t] / p[t - 1] - 1.0
    ret_5 = p[t] / p[t - 5] - 1.0
    ma_gap_10 = p[t] / np.mean(p[t - 9:t + 1]) - 1.0
    returns = p[t - 9:t + 1] / p[t - 10:t] - 1.0
    vol_10 = float(np.sqrt(np.mean((returns - returns.mean()) ** 2)))
    changes = p[t - 13:t + 1] - p[t - 14:t]
    gain = changes[changes > 0].sum()
    loss = -changes[changes < 0].sum()
    rsi = 100.0 if loss == 0 else 100.0 - 100.0 / (1.0 + (gain / 14.0) / (loss / 14.0))
    return [ret_1, ret_5, ma_gap_10, vol_10, (rsi - 50.0) / 50.0]


def run_bars(df: pd.DataFrame, bar_ms: int) -> dict[str, np.ndarray]:
    """
    Price samples of a run cut into bars: the last price of every bar_ms of simulated time (what the ML bot sees live)
    """
    df = df[df["last_price"] > 0].sort_values("elapsed_ms")
    series = {}
    for symbol, g in df.groupby("symbol"):
        if bar_ms > 0:
            bar = (g["elapsed_ms"] // bar_ms).to_numpy()
            g = g.groupby(bar).tail(1) # the close of each bar
        series[symbol] = g["last_price"].to_numpy(dtype=float)
    return series


def load_series(input_path: Path, suffix: str, bar_ms: int = 500) -> dict[str, np.ndarray]:
    """
    Price series per symbol, oldest first
    """
    if input_path.is_dir():
        csv = input_path / f"price_samples_{suffix}.csv"
        if not csv.exists():
            raise SystemExit(f"{display_path(csv)} not found: --input expects a simulation run folder (or a CSV of closes)")
        return run_bars(pd.read_csv(csv), bar_ms)
    if not input_path.exists():
        raise SystemExit(f"{display_path(input_path)} not found: fetch the real market first (scripts/refresh_data.sh small)")
    df = pd.read_csv(input_path)
    if "Close" in df.columns and "close" not in df.columns:
        df = df.rename(columns={"Close": "close"}) # features.csv
    if {"ticker", "date", "close"} <= set(df.columns):
        df = df[df["close"] > 0].sort_values("date")
        return {s: g["close"].to_numpy(dtype=float) for s, g in df.groupby("ticker")}
    if {"symbol", "elapsed_ms", "last_price"} <= set(df.columns):
        df = df[df["last_price"] > 0].sort_values("elapsed_ms")
        return {s: g["last_price"].to_numpy(dtype=float) for s, g in df.groupby("symbol")}
    raise SystemExit(f"{display_path(input_path)}: expected columns ticker, date, close (or Close), or symbol, elapsed_ms, last_price")


def build_dataset(series: dict[str, np.ndarray], horizon: int, train_fraction: float):
    x_train, y_train, x_test, y_test = [], [], [], []
    for symbol, p in series.items():
        rows = [(features_at(p, t), 1.0 if p[t + horizon] > p[t] else 0.0) for t in range(MIN_HISTORY - 1, len(p) - horizon)]
        if len(rows) < 10:
            print(f"  {symbol}: only {len(rows)} usable points, skipped")
            continue
        cut = int(len(rows) * train_fraction) # time-ordered split: the test part is strictly later
        for i, (x, y) in enumerate(rows):
            (x_train if i < cut else x_test).append(x)
            (y_train if i < cut else y_test).append(y)
    if not x_train or not x_test:
        raise SystemExit("not enough data to train and test: use a longer run (e.g. ./launch.sh simu --simu-duration 120)")
    return np.array(x_train), np.array(y_train), np.array(x_test), np.array(y_test)


def train_logistic(x: np.ndarray, y: np.ndarray, l2: float, epochs: int, learning_rate: float = 0.1):
    w = np.zeros(x.shape[1])
    b = 0.0
    n = len(y)
    for _ in range(epochs):
        p = 1.0 / (1.0 + np.exp(-(x @ w + b)))
        w -= learning_rate * ((x.T @ (p - y)) / n + l2 * w)
        b -= learning_rate * np.mean(p - y)
    return w, b


def feature_table(series: dict[str, np.ndarray]) -> np.ndarray:
    rows = [features_at(p, t) for p in series.values() for t in range(MIN_HISTORY - 1, len(p))]
    return np.array(rows) if rows else np.zeros((0, len(FEATURES)))


def check_run(train_series: dict[str, np.ndarray], train_path: Path, run: Path, suffix: str, bar_ms: int) -> None:
    """
    Training data vs a simulation run cut into bars: are the bot's live features in the range the model learned from?
    """
    if bar_ms <= 0:
        raise SystemExit("--check-run needs --bar-ms > 0 (the bar the ML bot uses, --ml-bar-ms in simulation.x)")
    train = feature_table(train_series)
    live = feature_table(load_series(run, suffix, bar_ms))
    if len(train) == 0 or len(live) == 0:
        raise SystemExit("not enough history in the training data or in the run (the run needs at least 15 bars per symbol)")
    print(f"training: {len(train)} points from {display_path(train_path)}, run: {len(live)} bars of {bar_ms} ms from {display_path(run)}")
    print(f"{'feature':12s} {'training std':>13s} {'run std':>10s} {'ratio':>7s}   (ratio near 1: the run looks like the training period)")
    for i, name in enumerate(FEATURES):
        a, b = train[:, i].std(), live[:, i].std()
        print(f"{name:12s} {a:13.5f} {b:10.5f} {b / a if a > 0 else float('nan'):7.2f}")
    ratio = live[:, 0].std() / train[:, 0].std() if train[:, 0].std() > 0 else float("nan")
    if np.isfinite(ratio) and ratio > 0:
        # returns grow with the square root of time: bar volatility x (T_new / T)^0.5 = daily volatility
        suggested = bar_ms / ratio ** 2
        print(f"one-bar returns are {ratio:.2f} times as volatile as one-day returns: a bar of about {suggested:.0f} ms would match a trading day (simulation.x --ml-bar-ms {max(1, int(round(suggested)))})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, default=None)
    parser.add_argument("--suffix", choices=("simu", "bots"), default="simu")
    parser.add_argument("--output", type=Path, default=SIM_DIR / "models" / "signal_model.csv")
    parser.add_argument("--horizon", type=int, default=5)
    parser.add_argument("--train-fraction", type=float, default=0.7)
    parser.add_argument("--l2", type=float, default=0.01)
    parser.add_argument("--epochs", type=int, default=2000)
    parser.add_argument("--bar-ms", type=int, default=500)
    parser.add_argument("--check-run", type=Path, default=None)
    args = parser.parse_args()

    if args.input is None: # the fetched real market: all_prices.csv, or features.csv
        args.input = DATA_PROCESSED / "all_prices.csv"
        if not args.input.exists() and (DATA_PROCESSED / "features.csv").exists():
            args.input = DATA_PROCESSED / "features.csv"
    series = load_series(args.input, args.suffix, args.bar_ms)
    if args.check_run is not None:
        check_run(series, args.input, args.check_run, args.suffix, args.bar_ms)
        return
    print(f"{len(series)} series from {display_path(args.input)}")
    x_train, y_train, x_test, y_test = build_dataset(series, args.horizon, args.train_fraction)

    mean = x_train.mean(axis=0)
    std = x_train.std(axis=0)
    std[std == 0] = 1.0 # same rule as the C++ loader
    w, b = train_logistic((x_train - mean) / std, y_train, args.l2, args.epochs)

    def accuracy(x, y):
        p = 1.0 / (1.0 + np.exp(-(((x - mean) / std) @ w + b)))
        return float(np.mean((p > 0.5) == (y > 0.5)))

    baseline = max(np.mean(y_test), 1.0 - np.mean(y_test))
    print(f"train {len(y_train)} points, test {len(y_test)} points (test = the later part of every series)")
    print(f"accuracy  train {accuracy(x_train, y_train):.3f}  test {accuracy(x_test, y_test):.3f} (naive baseline on test: {baseline:.3f})")
    for name, weight in zip(FEATURES, w):
        print(f"  weight {name:10s} {weight:+.4f}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w") as out:
        out.write("# signal model written by scripts/train_signal_model.py, loaded by SignalModel (signal_model.hpp)\n")
        out.write(f"# trained on {args.input.name}, test accuracy {accuracy(x_test, y_test):.3f} vs baseline {baseline:.3f}\n")
        out.write("kind,name,mean,std,weight\n")
        out.write(f"meta,horizon,{args.horizon},,\n")
        # plain floats at full precision: repr() of a numpy 2 scalar is "np.float64(...)", not a number
        out.write(f"bias,bias,,,{float(b):.17g}\n")
        for name, m, sd, weight in zip(FEATURES, mean, std, w):
            out.write(f"feature,{name},{float(m):.17g},{float(sd):.17g},{float(weight):.17g}\n")
    print(f"model written to {display_path(args.output)}")


if __name__ == "__main__":
    main()
