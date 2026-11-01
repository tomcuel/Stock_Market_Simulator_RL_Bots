"""
Is the recorded market a stock market? Its daily statistics, return and gap distributions and autocorrelation against
the real tickers it starts from, as charts (rl_agents.plots) in reports/<market>/plots/ with an index page

Usage (from RL_Agents/; scripts/simulate_market_days.sh runs it after each recording):
    python -m rl_agents.calibration --data sim_days/real5_d600
    python -m rl_agents.calibration --data sim_days/real5_d600 --real ../../Data/Datasets/processed/all_prices.csv --real-days 1250

The real tickers are the recorded market's own symbols, from the Data/ pipeline's daily history (their last --real-days days). 
The real spread isn't in that history, so the spread is compared by scripts/market_calibration.py only, against the reference market. 
The numeric checks (within 15% of the reference market, no thinning over time) stay in that script.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from . import plots
from .daily import market_statistics, run_seed
from .paths import display_path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REAL = ROOT.parent.parent / "Data" / "Datasets" / "processed" / "all_prices.csv"


def real_statistics(prices: pd.DataFrame) -> tuple[pd.DataFrame, list[np.ndarray], np.ndarray, np.ndarray]:
    """
    Per real ticker, the statistics that its daily history holds (drift, volatility, autocorrelation, range, gaps and their share of the variance), and its daily returns and gaps
    """
    rows, paths, returns, gaps = [], [], [], []
    for ticker, g in prices.groupby("ticker"):
        g = g.sort_values("date")
        r = np.log(g["close"]).diff().dropna()
        gap = np.log(g["open"] / g["close"].shift(1)).dropna()
        rows.append({"ticker": ticker, "drift_bp": 1e4 * r.mean(), "vol_bp": 1e4 * r.std(), "autocorr": r.autocorr(1),
                     "range_bp": 1e4 * ((g["high"] - g["low"]) / g["close"]).mean(), "abs_gap_bp": 1e4 * gap.abs().mean(),
                     "overnight_share": gap.var() / r.var()})
        paths.append(r.to_numpy())
        returns.append(r.to_numpy())
        gaps.append(gap.to_numpy())
    return pd.DataFrame(rows), paths, np.concatenate(returns) if returns else np.array([]), np.concatenate(gaps) if gaps else np.array([])


def simulated_series(path: Path) -> tuple[list[np.ndarray], np.ndarray, np.ndarray, list[str]]:
    """
    Every (market, symbol) path of daily returns, the pooled returns and gaps, the market's symbols
    """
    paths, returns, gaps, symbols = [], [], [], set()
    for folder in sorted((p for p in Path(path).iterdir() if (p / "daily_bars_simu.csv").exists()), key=run_seed):
        bars = pd.read_csv(folder / "daily_bars_simu.csv")
        for symbol, g in bars.groupby("symbol"):
            g = g.sort_values("day")
            r = np.log(g["close"]).diff().dropna().to_numpy()
            paths.append(r)
            returns.append(r)
            gaps.append(g["gap"].iloc[1:].to_numpy())
            symbols.add(symbol)
    return paths, np.concatenate(returns), np.concatenate(gaps), sorted(symbols)


def calibration_report(data: Path, real_path: Path = DEFAULT_REAL, real_days: int = 1250, out: Path | None = None, log=print) -> Path | None:
    data = Path(data).resolve()
    out = Path(out or ROOT / "reports" / data.name / "plots")
    sim_paths, sim_returns, sim_gaps, symbols = simulated_series(data)
    simulated = market_statistics(data)
    if Path(real_path).exists():
        prices = pd.read_csv(real_path, usecols=["ticker", "date", "open", "high", "low", "close"])
        prices = prices[prices["ticker"].isin(symbols)].sort_values(["ticker", "date"]).groupby("ticker").tail(real_days)
    else:
        log(f"no real history at {display_path(real_path)}: the simulated market alone")
        prices = pd.DataFrame(columns=["ticker", "date", "open", "high", "low", "close"])
    real, real_paths, real_returns, real_gaps = real_statistics(prices)
    plt = plots.plotting()
    if plt is None:
        return None
    plots.plot_calibration_stats(plt, simulated, real, out / "statistics.png")
    plots.plot_return_distributions(plt, sim_returns, sim_gaps, real_returns, real_gaps, out / "distributions.png")
    plots.plot_autocorrelation(plt, sim_paths, real_paths, out / "autocorrelation.png")
    page = plots.index_page(out, f"Calibration of {data.name}",
                            f"{len(simulated)} simulated markets of {display_path(data)} ({len(sim_paths)} symbol paths) against the real daily history of "
                            f"{', '.join(real['ticker']) if len(real) else 'no real ticker'} (their last {real_days} days, {display_path(real_path)}).", [
        ("Is it a stock market?", ["statistics.png"], "Each statistic of README section 2: every simulated market a blue dot (the median over its symbols), the white line their median, each real ticker an orange dot, the real range shaded."),
        ("Do its days move like real ones?", ["distributions.png"], "The daily returns and the overnight gaps, simulated and real, as densities on the same bins."),
        ("Can yesterday predict today?", ["autocorrelation.png"], "A random walk stays inside the grey band; the value investors pull the price back toward the hidden value, a slight reversion."),
    ])
    log(f"calibration charts: {display_path(page)}")
    return page


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, required=True, help="a recorded market (scripts/simulate_market_days.sh)")
    parser.add_argument("--real", type=Path, default=DEFAULT_REAL, help="the real daily history (the Data/ pipeline's all_prices.csv)")
    parser.add_argument("--real-days", type=int, default=1250, help="the real tickers' most recent days to compare with")
    parser.add_argument("--out", type=Path, default=None, help="where the charts go (default reports/<market>/plots)")
    args = parser.parse_args()
    calibration_report(args.data, args.real, args.real_days, args.out)


if __name__ == "__main__":
    main()
