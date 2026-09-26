#!/usr/bin/env python3
"""
Reads Datasets/processed/all_prices.csv (produced by preprocess.py) and computes, per ticker:
    daily_return, log_return, sma_20, sma_50, ema_20, rolling_volatility_20 (annualized), rsi_14, latest_price

Two things downstream consumers care about most:
  - `latest_price` + `rolling_volatility_20` are exactly what you'd hand to Src_Simulation.SymbolConfig (as `initial_price`) and to a bot's random price-offset distribution, 
    so simulated symbols start from and move around realistic real-world values  instead of arbitrary numbers
    --> extensions will surely come later when the Src_Simulation will be closer to a production-ready level
  - the full per-day feature table is what enter_in_database.py loads into the `prices` table of the SQL exchange's database, 
    so the market can be seeded with real historical price history instead of a single starting price

Usage:
    python3 feature_engineering.py
    python3 feature_engineering.py --input Datasets/processed/all_prices.csv --use-common-features
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

DATA_DIR = Path(__file__).resolve().parent
PROCESSED_DIR = DATA_DIR / "Datasets" / "processed"

TRADING_DAYS_PER_YEAR = 252

COMMON_FEATURE_COLS = ['Close', 
                    'SMA_50', 'EMA_20', 
                    'Momentum_4', 'Momentum_10',
                    'RSI_14', 'Stochastic_K', 'Stochastic_D', 
                    'Volatility_10', 
                    'MACD_Signal',
                    'Upper_Band', 'BB_%B'
                    ]


def _rsi(close: pd.Series, window: int = 14) -> pd.Series:
    delta = close.diff()
    gain = delta.clip(lower=0.0)
    loss = -delta.clip(upper=0.0)
    # Wilder's smoothing (equivalent to an EMA with alpha = 1/window), the standard RSI definition
    avg_gain = gain.ewm(alpha=1.0 / window, min_periods=window, adjust=False).mean()
    avg_loss = loss.ewm(alpha=1.0 / window, min_periods=window, adjust=False).mean()
    rs = avg_gain / avg_loss.replace(0.0, np.nan)
    rsi = 100.0 - (100.0 / (1.0 + rs))
    # an avg_loss of exactly 0 (a pure uptrend window) means RSI should read 100, not NaN
    rsi = rsi.where(avg_loss != 0.0, 100.0)
    return rsi


def compute_features(prices: pd.DataFrame, use_common_features: bool = True) -> pd.DataFrame:
    prices = prices.sort_values(["ticker", "date"]).copy()

    def per_ticker(group: pd.DataFrame) -> pd.DataFrame:
        group = group.copy()
        close = group["close"]
        group["Close"] = close
        group["Daily_Return"] = close.pct_change()
        # all_prices.csv (preprocess.py) has no log_return column: computed here, per ticker
        group["log_return"] = np.log(close / close.shift(1))
        group["Log_Return"] = group["log_return"]

        # Additional trend, momentum, volatility, and band features
        group["SMA_50"] = close.rolling(window=50).mean()
        group["SMA_200"] = close.rolling(window=200).mean()
        group["EMA_20"] = close.ewm(span=20, adjust=False).mean()
        group["EMA_50"] = close.ewm(span=50, adjust=False).mean()
        group["Momentum_4"] = close - close.shift(4)
        group["Momentum_10"] = close - close.shift(10)

        group["RSI_14"] = _rsi(close, window=14)

        low14 = close.rolling(14).min()
        high14 = close.rolling(14).max()
        group["Stochastic_K"] = 100 * (close - low14) / (high14 - low14)
        group["Stochastic_D"] = group["Stochastic_K"].rolling(3).mean()

        group["Volatility_10"] = close.rolling(10).std()
        group["Volatility_20"] = close.rolling(20).std()
        group["Rolling_Volatility_20"] = group["log_return"].rolling(20).std() * np.sqrt(TRADING_DAYS_PER_YEAR)

        exp12 = close.ewm(span=12, adjust=False).mean()
        exp26 = close.ewm(span=26, adjust=False).mean()
        group["MACD"] = exp12 - exp26
        group["MACD_Signal"] = group["MACD"].ewm(span=9, adjust=False).mean()

        group["Middle_Band"] = close.rolling(20).mean()
        rolling_std_20 = close.rolling(20).std()
        group["Upper_Band"] = group["Middle_Band"] + (rolling_std_20 * 2)
        group["Lower_Band"] = group["Middle_Band"] - (rolling_std_20 * 2)
        group["BB_%B"] = (close - group["Lower_Band"]) / (group["Upper_Band"] - group["Lower_Band"])

        if use_common_features:
            return group[["date"] + COMMON_FEATURE_COLS] # the date is kept so rows stay traceable
        return group

    features = prices.groupby("ticker", group_keys=False).apply(per_ticker, include_groups=False)
    features.insert(0, "ticker", prices["ticker"].values)
    return features

def summarize_latest(features: pd.DataFrame) -> pd.DataFrame:
    """
    One row per ticker: the most recent snapshot, useful for seeding Src_Simulation symbols
    """
    latest = features.sort_values(["ticker", "date"]).groupby("ticker").tail(1).reset_index(drop=True)
    # Src_Simulation's market_seed.cpp reads ticker, latest_price and rolling_volatility_20 by name
    summary = pd.DataFrame({
        "ticker": latest["ticker"],
        "date": latest["date"],
        "latest_price": latest["Close"],
        "rolling_volatility_20": latest["Rolling_Volatility_20"],
        "rsi_14": latest["RSI_14"],
        "sma_20": latest["Middle_Band"],
        "sma_50": latest["SMA_50"],
    })
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, default=PROCESSED_DIR / "all_prices.csv")
    parser.add_argument("--use-common-features", action="store_true", help="only compute the common features used by the ML model, instead of the full set of features")
    parser.add_argument("--output", type=Path, default=PROCESSED_DIR / "features.csv")
    parser.add_argument("--summary-output", type=Path, default=PROCESSED_DIR / "latest_snapshot.csv")
    args = parser.parse_args()
    if not args.input.exists():
        raise SystemExit(f"{args.input} not found. Run preprocess.py first.")

    prices = pd.read_csv(args.input, parse_dates=["date"])
    # the full set is always computed: the snapshot needs Rolling_Volatility_20 and Middle_Band even when
    # only the common features are written to features.csv
    all_features = compute_features(prices, use_common_features=False)
    features = all_features[["ticker", "date"] + COMMON_FEATURE_COLS] if args.use_common_features else all_features

    PROCESSED_DIR.mkdir(parents=True, exist_ok=True)
    features.to_csv(args.output, index=False)
    print(f"Wrote {len(features)} rows of features to {args.output}")

    summary = summarize_latest(all_features)
    summary.to_csv(args.summary_output, index=False)
    print(f"Wrote latest per-ticker snapshot ({len(summary)} tickers) to {args.summary_output}")


if __name__ == "__main__":
    main()
