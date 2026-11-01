#!/usr/bin/env python3
"""
plot_market_report.py: turns an end-of-run market report into trading-terminal style charts and one HTML page

Input: a run folder holding the report CSVs, produced by
    ./simulation.x --output-dir DIR      -> DIR/{summary,symbols,trades,...}_simu.csv
    ./sim_server.x --output-dir DIR      -> DIR/{summary,symbols,trades,...}_bots.csv  (on shutdown)
    ./launch.sh                          -> Src_RL_Trading/output/<timestamp>/ (both kinds)
Output, in DIR/plots/ (the launchers call this automatically):
    (with trading days, every time axis is labelled in trading days: a tick at the start of a day, named by its number)
    01_price_paths_<sfx>.png      every symbol's price rebased to 100, plus the bid-ask spread during continuous trading, on a log scale (with trading days, the auctions and the nights are shaded: there the book is crossed or emptied, and its "spread" is not one)
    02_before_after_<sfx>.png     % change per symbol, start vs end
    03_candles_<sfx>.png          OHLC candlesticks + volume per symbol: with trading days one candle per day (daily_bars: open = the opening auction, close = the closing auction, x = the trading day), otherwise built from the trade tape in --candle-seconds buckets
    04_depth_<sfx>.png            final order book depth per symbol (cumulative bids vs asks)
    05_book_pressure_<sfx>.png    order book imbalance heatmap (symbols x time)
    06_order_lifecycle_<sfx>.png  where orders ended up: filled / resting / waiting / expired / rejected, the rejections of the strategies apart from the value investors' (the informed benchmark, mostly sells of shares they no longer hold)
    07_portfolios_<sfx>.png       client net worth over time (the top and bottom clients among the strategies highlighted, the value investors in the grey background), P&L distribution, per-client before vs after
    08_trade_flow_<sfx>.png       volume and notional per symbol, trade sizes, activity over time
    09_waiting_orders_<sfx>.png   every still-waiting STOP/LIMIT_STOP order's release band vs final price
    10_strategies_<sfx>.png       what each strategy added over holding its starting account, one dot per bot: the strategies that only see prices ranked, the value investors apart (the informed benchmark: they see each company's hidden fundamental value, which moves the prices)
    report_<sfx>.html             summary tables + every chart above (+ metrics_<sfx>.png if present) where <sfx> is "simu" or "bots"

Usage (from Src_RL_Trading/):
    python3 scripts/plot_market_report.py output/latest --suffix simu   # folder with both kinds: pick one
    python3 scripts/plot_market_report.py output/my_run                 # folder with a single kind
    python3 scripts/plot_market_report.py output/latest --suffix bots --show

Options (all optional):
    --suffix simu|bots     which report to plot when the folder holds both
    --output-dir DIR       where to write the charts (default: <run folder>/plots)
    --candle-seconds S     candle width for a run without trading days (default: run duration / 40); with trading days the candles are the days themselves
    --highlight N          top/bottom clients highlighted in the portfolio chart (default 3)
    --show                 also open the charts in windows (needs a display)
"""
from __future__ import annotations

import argparse
import html
import math
import os
import re
import sys
from pathlib import Path

import numpy as np
import pandas as pd

TITLES = {"simu": "simulation (in-process)", "bots": "server + socket bots"}

UP = "#26a69a"    
DOWN = "#ef5350"
ACCENT = "#42a5f5"
MUTED = "#90a4ae"
BG = "#131722"
PANEL = "#1e222d"
GRID = "#2a2e39"
TEXT = "#d1d4dc"

# the value investors (simulation.x --value) see each company's hidden fundamental value, the variable that moves the prices: an informed benchmark, what knowing the value is worth, shown apart and never ranked with the strategies that only see prices
INFORMED = {"value"}


# ---------------------------------------------------------------------------------------------
# loading
# ---------------------------------------------------------------------------------------------
def display_path(path) -> str:
    """
    A path as shown to the user: relative to the project root (the folder holding Src_RL_Trading/ and Data/), e.g. Src_RL_Trading/output/20260925_012005, never absolute
    """
    resolved = Path(path).resolve()
    for parent in [resolved, *resolved.parents]:
        if (parent / "Src_RL_Trading").is_dir() and (parent / "Data").is_dir():
            return str(resolved.relative_to(parent))
    return str(path)


def read_csv(report_dir: Path, name: str) -> pd.DataFrame:
    path = report_dir / name
    if not path.exists():
        print(f"warning: {name} not found in {display_path(report_dir)}, related charts will be skipped", file=sys.stderr)
        return pd.DataFrame()
    return pd.read_csv(path)


KNOWN_SUFFIXES = ("simu", "bots")


def resolve_suffix(report_dir: Path, suffix: str | None) -> str:
    """
    Returns the report's file suffix ("simu"/"bots"), detecting it from summary_*.csv if not given
    """
    if suffix:
        if not (report_dir / f"summary_{suffix}.csv").exists():
            raise SystemExit(f"summary_{suffix}.csv not found in {display_path(report_dir)}")
        return suffix
    found = sorted(report_dir.glob("summary_*.csv"))
    if not found:
        raise SystemExit(f"no summary_*.csv in {display_path(report_dir)}: was the run started with --output-dir (and without --no-report)?")
    if len(found) > 1:
        names = ", ".join(f.name for f in found)
        raise SystemExit(f"{display_path(report_dir)} holds several reports ({names}): pick one with --suffix {'|'.join(KNOWN_SUFFIXES)}")
    return found[0].stem.split("_", 1)[1]


def load_report(report_dir: Path, suffix: str) -> dict[str, pd.DataFrame]:
    names = ["summary", "symbols", "trades", "price_samples", "portfolio_samples", "portfolios_final", "order_book_final", "resting_orders", "waiting_orders", "rejections"]
    data = {name: read_csv(report_dir, f"{name}_{suffix}.csv") for name in names}
    # written by newer runs only (simulation.x with trading days for the phases): read when present, no warning otherwise
    for name in ("rejections_by_client", "session_phases", "daily_bars"):
        path = report_dir / f"{name}_{suffix}.csv"
        data[name] = pd.read_csv(path) if path.exists() else pd.DataFrame()
    if data["symbols"].empty:
        raise SystemExit(f"{display_path(report_dir)} doesn't look like a market report (symbols_{suffix}.csv missing or empty)")
    for key in ("trades", "price_samples", "portfolio_samples"):
        if not data[key].empty:
            data[key]["elapsed_s"] = data[key]["elapsed_ms"] / 1000.0
    return data


def summary_value(summary: pd.DataFrame, key: str, default=0.0) -> float:
    if summary.empty:
        return default
    match = summary.loc[summary["key"] == key, "value"]
    return float(match.iloc[0]) if not match.empty else default


# ---------------------------------------------------------------------------------------------
# styling helpers
# ---------------------------------------------------------------------------------------------
def apply_style(plt) -> None:
    plt.rcParams.update({
        "figure.facecolor": BG, "axes.facecolor": PANEL, "savefig.facecolor": BG,
        "axes.edgecolor": GRID, "axes.labelcolor": TEXT, "axes.titlecolor": TEXT,
        "xtick.color": MUTED, "ytick.color": MUTED, "text.color": TEXT,
        "grid.color": GRID, "grid.linestyle": "-", "grid.linewidth": 0.6,
        "axes.grid": True, "legend.facecolor": PANEL, "legend.edgecolor": GRID,
        "font.size": 9, "axes.titlesize": 11, "axes.titleweight": "bold",
    })


def grid_shape(n: int, max_cols: int = 3) -> tuple[int, int]:
    cols = min(max_cols, max(1, n))
    return math.ceil(n / cols), cols


def hide_unused(axes_flat, used: int) -> None:
    for ax in axes_flat[used:]:
        ax.set_visible(False)


def symbol_colors(plt, symbols) -> dict[str, tuple]:
    cmap = plt.get_cmap("tab10" if len(symbols) <= 10 else "tab20")
    return {sym: cmap(i % cmap.N) for i, sym in enumerate(symbols)}


# ---------------------------------------------------------------------------------------------
# charts
# ---------------------------------------------------------------------------------------------
def phase_at(times_ms: np.ndarray, phases: pd.DataFrame) -> np.ndarray:
    """
    The session phase in effect at each time (session_phases: when each phase of each day started); CLOSED before the first day opens
    """
    if phases.empty:
        return np.full(len(times_ms), "", dtype=object)
    p = phases.sort_values("start_ms")
    starts, names = p["start_ms"].to_numpy(), p["phase"].to_numpy(dtype=object)
    k = np.searchsorted(starts, times_ms, side="right") - 1
    return np.where(k >= 0, names[np.clip(k, 0, None)], "CLOSED")


def closed_intervals(phases: pd.DataFrame, end_ms: float) -> list[tuple[float, float]]:
    """
    The stretches without continuous trading (pre-open, pre-close, night), in seconds
    """
    if phases.empty:
        return []
    p = phases.sort_values("start_ms").reset_index(drop=True)
    spans, start = [], 0.0
    for row in p.itertuples():
        if row.phase == "OPEN":
            spans.append((start, row.start_ms))
            start = None
        elif start is None:
            start = row.start_ms
    if start is not None:
        spans.append((start, end_ms))
    return [(a / 1000.0, b / 1000.0) for a, b in spans if b > a]


def day_axis(ax, data, label: bool = True) -> None:
    """Labels a time axis (seconds since the start of the run) in trading days: a tick at the start of every N-th day (its pre-open, from daily_bars' open_ms), about ten per axis, named by the day's number
    Only the labels change, the data stays in seconds, a run without trading days (the bots phase) keeps "elapsed seconds"
    """
    daily = data.get("daily_bars", pd.DataFrame()) if isinstance(data, dict) else data
    if daily is None or daily.empty or "open_ms" not in daily.columns:
        if label:
            ax.set_xlabel("elapsed seconds")
        return
    days = daily.drop_duplicates("day").sort_values("day")
    starts, numbers = days["open_ms"].to_numpy() / 1000.0, days["day"].to_numpy()
    step = next((k for k in (1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500) if len(numbers) / k <= 12), 1000)
    keep = (numbers % step) == 0
    ax.set_xticks(starts[keep])
    ax.set_xticklabels([str(int(n)) for n in numbers[keep]])
    if label:
        ax.set_xlabel("trading day")


def plot_price_paths(plt, data, out: Path):
    ps = data["price_samples"]
    if ps.empty:
        return None
    symbols = sorted(ps["symbol"].unique())
    colors = symbol_colors(plt, symbols)
    phases = data.get("session_phases", pd.DataFrame())
    fig, (ax_price, ax_spread) = plt.subplots(2, 1, figsize=(12, 8), sharex=True, gridspec_kw={"height_ratios": [3, 1.3]})
    fig.suptitle("Price paths (rebased to 100 at start) and bid-ask spread", fontsize=13, fontweight="bold")

    medians = []
    for sym in symbols:
        s = ps[ps["symbol"] == sym].sort_values("elapsed_s")
        first = s["last_price"].iloc[0]
        if first <= 0:
            continue
        rebased = s["last_price"] / first * 100.0
        ax_price.plot(s["elapsed_s"], rebased, color=colors[sym], linewidth=1.6, label=f"{sym}  {rebased.iloc[-1] - 100:+.2f}%")
        # the spread is only a spread in continuous trading: in the auctions the book collects orders without matching (it is crossed, bid above ask, most of the time), at night only the GTC orders are left, far apart or on one side only
        # Outside continuous trading the line is broken (NaN), and a crossed book (spread <= 0) has no logarithm
        mid = (s["best_bid"] + s["best_ask"]) / 2.0
        spread = ((s["best_ask"] - s["best_bid"]) / mid * 10_000).where((s["best_bid"] > 0) & (s["best_ask"] > 0))
        if not phases.empty:
            spread = spread.where(phase_at(s["elapsed_ms"].to_numpy(), phases) == "OPEN")
        spread = spread.where(spread > 0)
        if spread.notna().any():
            ax_spread.plot(s["elapsed_s"], spread, color=colors[sym], linewidth=0.9, alpha=0.85)
            medians.append(spread.median())
    for a, b in closed_intervals(phases, ps["elapsed_ms"].max()):
        ax_spread.axvspan(a, b, color=MUTED, alpha=0.28, linewidth=0)
    ax_price.axhline(100, color=MUTED, linestyle="--", linewidth=0.8)
    ax_price.set_ylabel("index (start = 100)")
    ax_price.legend(loc="upper left", ncol=min(4, len(symbols)), fontsize=8)
    ax_spread.set_yscale("log")
    if medians:
        median = float(np.median(medians))
        ax_spread.axhline(median, color=TEXT, linestyle="--", linewidth=0.8)
        ax_spread.text(0.995, median, f"median {median:.1f} bp", transform=ax_spread.get_yaxis_transform(), ha="right", va="bottom", fontsize=8, color=TEXT)
    where = "continuous trading only; shaded: the auctions and the nights" if not phases.empty else "two-sided, uncrossed book"
    ax_spread.set_title(f"spread, log scale ({where})", loc="left", fontsize=9, fontweight="normal")
    ax_spread.set_ylabel("spread (bps of mid)")
    day_axis(ax_spread, data)
    fig.tight_layout()
    return fig


def plot_before_after(plt, data, out: Path):
    sy = data["symbols"].sort_values("change_pct")
    fig, ax = plt.subplots(figsize=(11, max(3.5, 0.55 * len(sy) + 1.5)))
    colors = [UP if v >= 0 else DOWN for v in sy["change_pct"]]
    bars = ax.barh(sy["symbol"], sy["change_pct"], color=colors, edgecolor=BG)
    ax.axvline(0, color=MUTED, linewidth=0.8)
    span = max(1e-9, sy["change_pct"].abs().max())
    for bar, (_, row) in zip(bars, sy.iterrows()):
        x = bar.get_width()
        ha = "left" if x >= 0 else "right"
        ax.text(x + (0.02 * span if x >= 0 else -0.02 * span), bar.get_y() + bar.get_height() / 2, f"{row['initial_price']:.2f} -> {row['final_price']:.2f}  ({x:+.2f}%)", va="center", ha=ha, fontsize=8, color=TEXT)
    ax.set_xlim(-span * 1.9 if (sy["change_pct"] < 0).any() else -span * 0.1, span * 1.9)
    ax.set_xlabel("% change, first to last price")
    ax.set_title("Before vs after: price change per symbol")
    fig.tight_layout()
    return fig


def build_candles(trades: pd.DataFrame, candle_s: float) -> pd.DataFrame:
    t = trades.sort_values("elapsed_ms").copy()
    t["bucket"] = (t["elapsed_s"] // candle_s).astype(int)
    grouped = t.groupby(["symbol", "bucket"])
    candles = grouped.agg(open=("price", "first"), high=("price", "max"), low=("price", "min"), close=("price", "last"), volume=("quantity", "sum")).reset_index()
    candles["t"] = candles["bucket"] * candle_s
    return candles


def plot_candles(plt, data, out: Path, candle_s: float | None):
    daily = data.get("daily_bars", pd.DataFrame())
    if not daily.empty:
        # trading days: one candle per day, the day's own bar (the official open and close come from the auctions), so the chart reads like a real stock's daily chart, gaps at the open included
        candles = daily.rename(columns={"day": "t"})[["symbol", "t", "open", "high", "low", "close", "volume"]]
        candles = candles[(candles["open"] > 0) & (candles["close"] > 0)]
        candle_s, duration, x_label = 1.0, float(candles["t"].max()) + 1.0, "trading day"
        title = f"Daily candles: one per trading day ({int(duration)} days; open: the opening auction, close: the closing auction) with volume"
        x0 = -0.5
    else:
        trades = data["trades"]
        if trades.empty:
            return None
        duration = max(trades["elapsed_s"].max(), 1e-3)
        if candle_s is None:
            candle_s = max(duration / 40.0, 0.05) # ~40 candles over the run by default
        candles = build_candles(trades, candle_s)
        x_label = "elapsed seconds"
        title = f"OHLC candles from the trade tape ({candle_s:.2f}s per candle) with volume"
        x0 = 0.0
    symbols = sorted(candles["symbol"].unique())
    rows, cols = grid_shape(len(symbols), 2)
    fig = plt.figure(figsize=(7 * cols, 4.2 * rows))
    fig.suptitle(title, fontsize=13, fontweight="bold")
    outer = fig.add_gridspec(rows, cols, hspace=0.45, wspace=0.18)
    width = candle_s * 0.7
    for i, sym in enumerate(symbols):
        inner = outer[i // cols, i % cols].subgridspec(2, 1, height_ratios=[3, 1], hspace=0.05)
        ax = fig.add_subplot(inner[0])
        ax_vol = fig.add_subplot(inner[1], sharex=ax)
        c = candles[candles["symbol"] == sym]
        center = 0.0 if not daily.empty else candle_s / 2 # a day's candle sits on its day number
        for _, k in c.iterrows():
            color = UP if k["close"] >= k["open"] else DOWN
            ax.vlines(k["t"] + center, k["low"], k["high"], color=color, linewidth=0.9)
            body_low = min(k["open"], k["close"])
            body_height = max(abs(k["close"] - k["open"]), (k["high"] - k["low"]) * 0.02 + 1e-9)
            ax.add_patch(plt.Rectangle((k["t"] + center - width / 2, body_low), width, body_height, facecolor=color, edgecolor=color, linewidth=0.6))
            ax_vol.bar(k["t"] + center, k["volume"], width=width, color=color, alpha=0.6)
        ax.set_xlim(x0, duration + (0.5 if not daily.empty else candle_s))
        pad = (c["high"].max() - c["low"].min()) * 0.08 or 1.0
        ax.set_ylim(c["low"].min() - pad, c["high"].max() + pad)
        first, last = c["open"].iloc[0], c["close"].iloc[-1]
        ax.set_title(f"{sym}   {last:.2f}  ({(last - first) / first * 100:+.2f}%)", loc="left", color=UP if last >= first else DOWN)
        ax.tick_params(labelbottom=False)
        ax_vol.set_ylabel("vol", fontsize=8)
        ax_vol.set_xlabel(x_label, fontsize=8)
    return fig


def plot_depth(plt, data, out: Path):
    book = data["order_book_final"]
    sy = data["symbols"].set_index("symbol")
    symbols = sorted(sy.index)
    rows, cols = grid_shape(len(symbols), 3)
    fig, axes = plt.subplots(rows, cols, figsize=(6 * cols, 4 * rows), squeeze=False)
    fig.suptitle("Final order book depth (cumulative quantity): bids vs asks", fontsize=13, fontweight="bold")
    flat = axes.flatten()
    for ax, sym in zip(flat, symbols):
        b = book[(book["symbol"] == sym) & (book["side"] == "BUY")].sort_values("price", ascending=False) if not book.empty else pd.DataFrame()
        a = book[(book["symbol"] == sym) & (book["side"] == "SELL")].sort_values("price") if not book.empty else pd.DataFrame()
        final_price = sy.loc[sym, "final_price"]
        # each side's last level is extended by `pad` toward the chart edge, so even a single price level shows up as a filled step (without it, one level renders as a bare vertical line)
        all_prices = pd.concat([b["price"] if not b.empty else pd.Series(dtype=float), a["price"] if not a.empty else pd.Series(dtype=float), pd.Series([final_price])])
        pad = max(all_prices.max() - all_prices.min(), abs(final_price) * 0.004, 1e-6) * 0.25
        if not b.empty:
            xs = list(b["price"]) + [b["price"].iloc[-1] - pad]
            ys = list(b["quantity"].cumsum()) + [b["quantity"].sum()]
            ax.step(xs, ys, where="post", color=UP, linewidth=1.5, label=f"bids ({int(b['quantity'].sum())})")
            ax.fill_between(xs, ys, step="post", color=UP, alpha=0.25)
        if not a.empty:
            xs = list(a["price"]) + [a["price"].iloc[-1] + pad]
            ys = list(a["quantity"].cumsum()) + [a["quantity"].sum()]
            ax.step(xs, ys, where="post", color=DOWN, linewidth=1.5, label=f"asks ({int(a['quantity'].sum())})")
            ax.fill_between(xs, ys, step="post", color=DOWN, alpha=0.25)
        ax.set_ylim(bottom=0)
        ax.axvline(final_price, color=ACCENT, linestyle="--", linewidth=1, label=f"last {final_price:.2f}")
        imbalance = sy.loc[sym, "imbalance"]
        ax.set_title(f"{sym}  imbalance {imbalance:+.2f}", loc="left", color=UP if imbalance >= 0 else DOWN)
        if b.empty and a.empty:
            ax.text(0.5, 0.5, "empty book", transform=ax.transAxes, ha="center", va="center", color=MUTED)
        ax.set_xlabel("price")
        ax.set_ylabel("cumulative qty")
        ax.legend(fontsize=7, loc="upper center")
    hide_unused(flat, len(symbols))
    fig.tight_layout()
    return fig


def plot_book_pressure(plt, data, out: Path):
    ps = data["price_samples"]
    if ps.empty:
        return None
    ps = ps.copy()
    total = ps["bid_depth"] + ps["ask_depth"]
    ps["imbalance"] = ((ps["bid_depth"] - ps["ask_depth"]) / total.where(total > 0)).fillna(0.0)
    pivot = ps.pivot_table(index="symbol", columns="elapsed_s", values="imbalance", aggfunc="mean").sort_index()
    fig, (ax_heat, ax_line) = plt.subplots(2, 1, figsize=(12, 3 + 0.45 * len(pivot) + 3.5), gridspec_kw={"height_ratios": [max(1.5, 0.45 * len(pivot)), 2.5]})
    fig.suptitle("Order book pressure: (bid depth - ask depth) / total depth, top of book", fontsize=13, fontweight="bold")
    times = pivot.columns.to_numpy()
    extent = [times.min(), times.max() if times.max() > times.min() else times.min() + 1, len(pivot) - 0.5, -0.5]
    im = ax_heat.imshow(pivot.to_numpy(), aspect="auto", cmap="RdYlGn", vmin=-1, vmax=1, extent=extent, interpolation="nearest")
    ax_heat.set_yticks(range(len(pivot)))
    ax_heat.set_yticklabels(pivot.index)
    ax_heat.grid(False)
    ax_heat.set_title("green = more resting bids (buy pressure), red = more resting asks (sell pressure)", fontsize=9, fontweight="normal")
    cbar = fig.colorbar(im, ax=ax_heat, pad=0.01)
    cbar.ax.tick_params(colors=MUTED)

    mean_imbalance = ps.groupby("elapsed_s")["imbalance"].mean()
    ax_line.fill_between(mean_imbalance.index, mean_imbalance.values, 0, where=mean_imbalance.values >= 0, color=UP, alpha=0.5, interpolate=True)
    ax_line.fill_between(mean_imbalance.index, mean_imbalance.values, 0, where=mean_imbalance.values < 0, color=DOWN, alpha=0.5, interpolate=True)
    ax_line.plot(mean_imbalance.index, mean_imbalance.values, color=TEXT, linewidth=0.8)
    ax_line.axhline(0, color=MUTED, linewidth=0.8)
    ax_line.set_ylim(-1, 1)
    ax_line.set_xlim(extent[0], extent[1]) # same time axis as the heatmap above
    ax_line.set_ylabel("market-wide mean imbalance")
    day_axis(ax_heat, data, label=False)
    day_axis(ax_line, data)
    fig.tight_layout()
    return fig


def rejections_by_group(data) -> pd.DataFrame | None:
    """
    Rejections per reason, split between the strategies that only see prices and the informed benchmark (the value investors): None when the run has no per-client file
    """
    by_client = data.get("rejections_by_client", pd.DataFrame())
    if by_client.empty:
        return None
    final = data["portfolios_final"]
    labels = final.set_index("client")["label"].map(strategy_of) if not final.empty else pd.Series(dtype=object)
    group = by_client["client"].map(labels).isin(INFORMED).map({True: "informed", False: "strategies"})
    return by_client.assign(group=group).pivot_table(index="reason", columns="group", values="count", aggfunc="sum", fill_value=0)


def plot_order_lifecycle(plt, data, out: Path):
    summary = data["summary"]
    submitted = summary_value(summary, "orders_submitted")
    filled_trades = summary_value(summary, "trades")
    resting = summary_value(summary, "resting_orders_final")
    waiting = summary_value(summary, "waiting_orders_final")
    expired = summary_value(summary, "orders_expired")
    rejected = summary_value(summary, "orders_rejected")
    queued = summary_value(summary, "orders_queued")
    groups = rejections_by_group(data)
    informed = float(groups["informed"].sum()) if groups is not None and "informed" in groups else 0.0

    fig = plt.figure(figsize=(16, 5.6))
    grid = fig.add_gridspec(1, 3, width_ratios=[2, 1.15, 1.15], wspace=0.45)
    ax_status, ax_reject, ax_informed = fig.add_subplot(grid[0]), fig.add_subplot(grid[1]), fig.add_subplot(grid[2])
    fig.suptitle(f"Order lifecycle: {int(submitted)} orders submitted", fontsize=13, fontweight="bold")
    labels = ["trades\nexecuted", "resting\nin book (end)", "waiting\nSTOP (end)", "ever\nqueued", "expired", "rejected"]
    values = [filled_trades, resting, waiting, queued, expired, rejected - informed]
    colors = [UP, ACCENT, "#ab47bc", "#7e57c2", "#ffa726", DOWN]
    bars = ax_status.bar(labels, values, color=colors, edgecolor=BG)
    if informed > 0:
        # the value investors' rejections stacked on top, in grey: mostly sells of shares they no longer hold (each one keeps its opinion of a stock, sells it out, then keeps trying), not the exchange refusing the strategies
        ax_status.bar(labels[-1], informed, bottom=values[-1], color=MUTED, alpha=0.55, edgecolor=BG, label="value investors (informed benchmark)")
        ax_status.legend(fontsize=8, loc="upper right")
    for bar, value in zip(bars, values):
        ax_status.text(bar.get_x() + bar.get_width() / 2, bar.get_height(), f"{int(value)}", ha="center", va="bottom", fontsize=9)
    if informed > 0:
        ax_status.text(bars[-1].get_x() + bars[-1].get_width() / 2, values[-1] + informed, f"+{int(informed)} value", ha="center", va="bottom", fontsize=9, color=MUTED)
    ax_status.tick_params(axis="x", labelsize=8)
    ax_status.set_ylabel("count")
    ax_status.set_title("completed vs still open at the end" + ("\nrejected: the strategies in red, the value investors in grey" if informed > 0 else ""), loc="left", fontsize=10)

    def reasons(ax, series: pd.Series | None, color, title: str, empty: str) -> None:
        if series is None or series.sum() == 0:
            ax.text(0.5, 0.5, empty, transform=ax.transAxes, ha="center", va="center", color=MUTED, fontsize=11)
            ax.set_xticks([])
            ax.set_yticks([])
        else:
            r = series[series > 0].sort_values()
            ax.barh(r.index, r.values, color=color, edgecolor=BG)
            for y, v in enumerate(r.values):
                ax.text(v, y, f" {int(v)}", va="center", fontsize=8)
            ax.set_xlabel("count")
        ax.set_title(title, loc="left", fontsize=10)

    if groups is not None:
        reasons(ax_reject, groups.get("strategies"), DOWN, "the strategies' rejections\n(they only see prices)", "no rejected orders")
        reasons(ax_informed, groups.get("informed"), MUTED, "the value investors' rejections\n(informed benchmark, own scale)", "no value investors\nor no rejection")
    else:
        rejections = data["rejections"]
        series = rejections.set_index("reason")["count"] if not rejections.empty else None
        reasons(ax_reject, series, DOWN, "rejections by reason (every client)", "no rejected orders")
        reasons(ax_informed, None, MUTED, "per client", "this run has no\nrejections_by_client file:\nre-run it to split them")
    fig.tight_layout()
    return fig


def plot_portfolios(plt, data, out: Path, highlight: int):
    samples = data["portfolio_samples"]
    final = data["portfolios_final"]
    if samples.empty or final.empty:
        return None
    fig = plt.figure(figsize=(14, 9))
    fig.suptitle("Client portfolios: net worth evolution and P&L", fontsize=13, fontweight="bold")
    gs = fig.add_gridspec(2, 2, height_ratios=[1.4, 1])
    ax_line = fig.add_subplot(gs[0, :])
    ax_hist = fig.add_subplot(gs[1, 0])
    ax_scatter = fig.add_subplot(gs[1, 1])

    # the top and bottom clients among the strategies that only see prices: the value investors (the informed benchmark) are never ranked, they stay in the grey background with everyone else
    strategy = final.set_index("client")["label"].map(strategy_of) if "label" in final.columns else pd.Series(dtype=object)
    ranked = final[~final["client"].map(strategy).isin(INFORMED)].sort_values("pnl")
    losers = list(ranked["client"].head(highlight))
    winners = list(ranked["client"].tail(highlight))[::-1]
    lines = {}
    for client, s in samples.groupby("client"):
        s = s.sort_values("elapsed_s")
        base = s["net_worth"].iloc[0]
        lines[client] = (s["elapsed_s"], (s["net_worth"] / base - 1.0) * 100.0 if base else s["net_worth"] * 0)
    for client, (x, rel) in lines.items():
        if client not in winners and client not in losers:
            ax_line.plot(x, rel, color=MUTED, linewidth=0.6, alpha=0.35)
    def name(client) -> str:
        label = strategy.get(client, "")
        return f"client {client}" + (f" ({label})" if isinstance(label, str) and label else "")
    for group, color in ((winners, UP), (losers, DOWN)): # drawn last, over the grey background
        for client in group:
            if client in lines:
                ax_line.plot(*lines[client], color=color, linewidth=1.8, label=name(client))
    ax_line.axhline(0, color=TEXT, linewidth=0.8)
    ax_line.set_ylabel("net worth change since first seen (%)")
    day_axis(ax_line, data)
    informed_note = ", the value investors (informed benchmark, not ranked) with them" if final["client"].map(strategy).isin(INFORMED).any() else ""
    ax_line.set_title(f"top {highlight} (green) and bottom {highlight} (red) of the strategies by P&L, everyone else in grey{informed_note}", loc="left", fontsize=9, fontweight="normal")
    ax_line.legend(fontsize=7, ncol=2, loc="upper left")

    pnl = final["pnl_pct"]
    bins = min(30, max(5, len(pnl) // 2))
    counts, edges, patches = ax_hist.hist(pnl, bins=bins, edgecolor=BG)
    for patch, left in zip(patches, edges[:-1]):
        patch.set_facecolor(UP if left >= 0 else DOWN)
    ax_hist.axvline(0, color=TEXT, linewidth=0.8)
    ax_hist.set_xlabel("P&L (%)")
    ax_hist.set_ylabel("clients")
    ax_hist.set_title(f"P&L distribution  (median {pnl.median():+.2f}%)", loc="left")

    # before vs after per client, as sorted P&L bars: a before/after scatter degenerates into a single vertical line whenever every client starts with the same net worth (e.g. every bot)
    ranked_pnl = final.sort_values("pnl", ascending=False).reset_index(drop=True)
    ax_scatter.bar(range(len(ranked_pnl)), ranked_pnl["pnl"], color=[UP if v >= 0 else DOWN for v in ranked_pnl["pnl"]], edgecolor=BG)
    ax_scatter.axhline(0, color=TEXT, linewidth=0.8)
    if len(ranked_pnl) <= 30:
        ax_scatter.set_xticks(range(len(ranked_pnl)))
        ax_scatter.set_xticklabels(ranked_pnl["client"].astype(str), fontsize=7)
    ax_scatter.set_xlabel("client (sorted by P&L)")
    ax_scatter.set_ylabel("net worth after - before")
    ax_scatter.set_title("before vs after, per client", loc="left")

    fig.tight_layout()
    return fig


def plot_trade_flow(plt, data, out: Path):
    trades = data["trades"]
    sy = data["symbols"].sort_values("volume", ascending=False)
    if trades.empty:
        return None
    colors = symbol_colors(plt, sorted(sy["symbol"]))
    fig, axes = plt.subplots(2, 2, figsize=(13, 8))
    fig.suptitle(f"Trade flow: {len(trades)} trades", fontsize=13, fontweight="bold")

    ax = axes[0, 0]
    ax.bar(sy["symbol"], sy["volume"], color=[colors[s] for s in sy["symbol"]])
    ax.set_title("volume (shares) per symbol", loc="left")

    ax = axes[0, 1]
    ax.bar(sy["symbol"], sy["notional"], color=[colors[s] for s in sy["symbol"]])
    ax.set_title("notional traded per symbol", loc="left")

    ax = axes[1, 0]
    ax.hist(trades["quantity"], bins=range(1, int(trades["quantity"].max()) + 2), color=ACCENT, align="left") #edgecolor=BG, align="left")
    ax.set_xlabel("trade size (shares)")
    ax.set_title("trade size distribution", loc="left")

    ax = axes[1, 1]
    for sym, t in trades.sort_values("elapsed_s").groupby("symbol"):
        ax.plot(t["elapsed_s"], range(1, len(t) + 1), color=colors.get(sym, ACCENT), linewidth=1.3, label=sym)
    day_axis(ax, data)
    ax.set_ylabel("cumulative trades")
    ax.set_title("trading activity over time", loc="left")
    ax.legend(fontsize=7)
    fig.tight_layout()
    return fig


def plot_waiting_orders(plt, data, out: Path):
    waiting = data["waiting_orders"]
    sy = data["symbols"].set_index("symbol")
    if waiting.empty:
        return None
    symbols = sorted(waiting["symbol"].unique())
    rows, cols = grid_shape(len(symbols), 3)
    fig, axes = plt.subplots(rows, cols, figsize=(6 * cols, 4 * rows), squeeze=False)
    fig.suptitle("Still-waiting STOP / LIMIT_STOP orders: release band vs final price", fontsize=13, fontweight="bold")
    flat = axes.flatten()
    for ax, sym in zip(flat, symbols):
        w = waiting[waiting["symbol"] == sym].sort_values("price").reset_index(drop=True)
        final_price = sy.loc[sym, "final_price"] if sym in sy.index else w["price"].median()
        span = max((w["price"].max() - w["price"].min()), final_price * 0.02)
        bar_width = max(0.8, min(3.0, 90.0 / max(1, len(w)))) # thinner bars when many orders share a panel
        for i, o in w.iterrows():
            color = UP if o["side"] == "BUY" else DOWN
            lower = o["release_lower"] if pd.notna(o["release_lower"]) else final_price - span * 1.5
            upper = o["release_upper"] if pd.notna(o["release_upper"]) else final_price + span * 1.5
            ax.hlines(i, lower, upper, color=color, linewidth=bar_width, alpha=0.45)
            ax.plot(o["price"], i, marker="D", color=color, markersize=4)
        ax.axvline(final_price, color=ACCENT, linestyle="--", linewidth=1.2, label=f"last {final_price:.2f}")
        ax.set_yticks([])
        ax.set_xlabel("price")
        buys, sells = (w["side"] == "BUY").sum(), (w["side"] == "SELL").sum()
        ax.set_title(f"{sym}  {buys} buy / {sells} sell waiting", loc="left")
        ax.legend(fontsize=7, loc="lower right")
    hide_unused(flat, len(symbols))
    fig.text(0.5, 0.005, "bar = release band (order enters the book once the price is inside it), diamond = order's limit price, green = BUY, red = SELL", ha="center", fontsize=8, color=MUTED)
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    return fig


def strategy_of(label) -> str:
    """
    Strategy behind a client label: the strategy name for simulation.x bots, and the part after "botNN_" for launch_bots.sh accounts (bot07_trend -> trend). Other accounts keep their name
    """
    label = "" if pd.isna(label) else str(label)
    match = re.match(r"^bot\d+_(.+)$", label)
    return match.group(1) if match else label


def risk_per_client(samples: pd.DataFrame) -> pd.DataFrame:
    """
    Per client, from its net worth sampled during the run: the Sharpe ratio of its sample-to-sample
    returns (per sample, not annualized: a run lasts seconds) and its maximum drawdown in percent
    """
    rows = []
    if samples is None or samples.empty:
        return pd.DataFrame(columns=["client", "sharpe", "max_drawdown_pct"])
    for client, g in samples.sort_values("elapsed_ms").groupby("client"):
        worth = g["net_worth"].to_numpy(dtype=float)
        worth = worth[worth > 0]
        if len(worth) < 3:
            continue
        r = np.diff(np.log(worth))
        sharpe = float(r.mean() / r.std()) if r.std() > 0 else 0.0
        drawdown = float((worth / np.maximum.accumulate(worth) - 1).min() * 100)
        rows.append({"client": client, "sharpe": sharpe, "max_drawdown_pct": drawdown})
    return pd.DataFrame(rows)


def excess_over_holding(final: pd.DataFrame, symbols: pd.DataFrame) -> pd.Series:
    """
    Per client: its final net worth minus what its starting account, held unchanged, is worth at the final prices
    Every bot starts balanced (half cash, the rest split equally by value across the symbols, at the run's initial prices), so the held account is rebuilt from the initial cash, the initial net worth and the symbols' initial and final prices
    Exact for simulation.x; for a sim_server.x account registered later in the session, approximate
    The raw P&L is mostly the market's own move: every bot holds stock from the start
    """
    if final.empty or symbols.empty or not {"initial_price", "final_price"} <= set(symbols.columns):
        return pd.Series(np.nan, index=final.index)
    growth = (symbols["final_price"] / symbols["initial_price"]).replace([np.inf, -np.inf], np.nan).dropna()
    if growth.empty:
        return pd.Series(np.nan, index=final.index)
    held = final["initial_cash"] + (final["initial_net_worth"] - final["initial_cash"]) * growth.mean()
    return final["final_net_worth"] - held


def strategy_table(final: pd.DataFrame, samples: pd.DataFrame | None = None, symbols: pd.DataFrame | None = None) -> pd.DataFrame:
    """
    Per strategy: what its bots added over holding their starting account (excess, the measure that compares strategies), their raw P&L, and the risk metrics of their bots during the run (Sharpe per sample, maximum drawdown)
    `informed` marks the value investors; the others are sorted by mean excess
    """
    if final.empty or "label" not in final.columns:
        return pd.DataFrame()
    df = final.assign(strategy=final["label"].map(strategy_of))
    df["excess_pct"] = 100 * excess_over_holding(df, symbols if symbols is not None else pd.DataFrame()) / df["initial_net_worth"]
    df = df[df["strategy"] != ""]
    if df.empty:
        return pd.DataFrame()
    risk = risk_per_client(samples)
    if not risk.empty:
        df = df.merge(risk, on="client", how="left")
    else:
        df = df.assign(sharpe=np.nan, max_drawdown_pct=np.nan)
    table = (df.groupby("strategy")
               .agg(bots=("client", "count"), mean_excess_pct=("excess_pct", "mean"), mean_pnl_pct=("pnl_pct", "mean"),
                    best_pnl_pct=("pnl_pct", "max"), worst_pnl_pct=("pnl_pct", "min"), sharpe=("sharpe", "mean"),
                    max_drawdown_pct=("max_drawdown_pct", "min"), trades=("trades_as_buyer", "sum")))
    table["informed"] = table.index.isin(INFORMED)
    key = "mean_excess_pct" if table["mean_excess_pct"].notna().any() else "mean_pnl_pct"
    return table.sort_values(["informed", key], ascending=[True, False])


def plot_strategies(plt, data, out: Path):
    final = data["portfolios_final"]
    table = strategy_table(final, symbols=data["symbols"])
    if table.empty:
        return None
    df = final.assign(strategy=final["label"].map(strategy_of))
    df["excess_pct"] = 100 * excess_over_holding(df, data["symbols"]) / df["initial_net_worth"]
    measure, label = ("excess_pct", "mean_excess_pct") if table["mean_excess_pct"].notna().any() else ("pnl_pct", "mean_pnl_pct")
    ranked = [s for s in table.index if not table.loc[s, "informed"]]
    informed = [s for s in table.index if table.loc[s, "informed"]]
    groups = [(ranked, "strategies that only see prices, ranked")] + ([(informed, "informed benchmark: sees the hidden fundamental value (not ranked)")] if informed else [])
    fig, axes = plt.subplots(len(groups), 1, figsize=(11, max(3.5, 0.6 * len(table) + 1.5 + 0.8 * (len(groups) - 1))), sharex=True,
                             gridspec_kw={"height_ratios": [max(len(g), 1) for g, _ in groups]}, squeeze=False)
    fig.suptitle("What each strategy added over holding its starting account: every dot is one bot, the bar is the mean",
                 fontsize=13, fontweight="bold")
    for ax, (order, title) in zip(axes[:, 0], groups):
        for y, strategy in enumerate(order):
            mean = table.loc[strategy, label]
            ax.barh(y, mean, color=UP if mean >= 0 else DOWN, alpha=0.35, edgecolor=BG)
            points = df.loc[df["strategy"] == strategy, measure]
            jitter = (pd.Series(range(len(points))) % 5 - 2) * 0.06
            ax.scatter(points, [y + j for j in jitter], s=22, color=[UP if v >= 0 else DOWN for v in points], edgecolors=BG, zorder=3)
            # labels in a fixed column just right of the plot area (axes coordinates), so they never collide with the strategy names, whatever the sign of the mean
            ax.text(1.01, y, f"{mean:+.2f}%  ({int(table.loc[strategy, 'bots'])} bots)", transform=ax.get_yaxis_transform(), va="center", ha="left", fontsize=9, color=UP if mean >= 0 else DOWN)
        ax.axvline(0, color=TEXT, linewidth=0.8)
        ax.set_yticks(range(len(order)))
        ax.set_yticklabels(order)
        ax.invert_yaxis()
        ax.set_title(title, loc="left", fontsize=10, color=MUTED)
    axes[-1, 0].set_xlabel("excess over holding the starting account (% of it), marked to the final prices" if measure == "excess_pct"
                           else "P&L since first seen (%), marked to the final prices")
    fig.tight_layout(rect=(0, 0, 0.86, 1)) # room on the right for the label column
    return fig


# ---------------------------------------------------------------------------------------------
# text summary + HTML
# ---------------------------------------------------------------------------------------------
def print_summary(data) -> None:
    summary = data["summary"]
    sy = data["symbols"]
    final = data["portfolios_final"]
    print("=" * 78)
    print(f"Market report: {summary_value(summary, 'duration_s'):.1f}s, {int(summary_value(summary, 'symbols'))} symbols, {int(summary_value(summary, 'clients'))} clients")
    print("=" * 78)
    print(f"orders submitted {int(summary_value(summary, 'orders_submitted'))}, trades {int(summary_value(summary, 'trades'))}, volume {int(summary_value(summary, 'volume'))}, notional {summary_value(summary, 'notional'):,.2f}")
    print(f"end state: {int(summary_value(summary, 'resting_orders_final'))} resting, {int(summary_value(summary, 'waiting_orders_final'))} waiting, {int(summary_value(summary, 'orders_expired'))} expired, {int(summary_value(summary, 'orders_rejected'))} rejected")
    cash_in, cash_out = summary_value(summary, "total_cash_initial"), summary_value(summary, "total_cash_final")
    print(f"cash conservation check: {cash_in:,.2f} before, {cash_out:,.2f} after ({'OK' if abs(cash_in - cash_out) < 1e-6 * max(1.0, cash_in) else 'MISMATCH'})")
    print("\nPer symbol, before -> after:")
    cols = ["symbol", "initial_price", "final_price", "change_pct", "trades", "volume", "vwap", "spread", "imbalance", "resting_orders", "waiting_orders"]
    print(sy[cols].to_string(index=False, float_format=lambda v: f"{v:,.2f}"))
    table = strategy_table(final, data.get("portfolio_samples"), sy)
    if not table.empty:
        print("\nStrategies that only see prices, ranked by what they added over holding their starting account (excess and P&L")
        print("in %, Sharpe per sample, worst drawdown of its bots in %):")
        print(table[~table["informed"]].drop(columns="informed").to_string(float_format=lambda v: f"{v:+.2f}"))
        if table["informed"].any():
            print("\nInformed benchmark (sees each company's hidden fundamental value, not ranked with the strategies):")
            print(table[table["informed"]].drop(columns="informed").to_string(float_format=lambda v: f"{v:+.2f}"))
    if not final.empty:
        print("\nTop 5 / bottom 5 clients by P&L:")
        ranked = final.sort_values("pnl", ascending=False)
        view = pd.concat([ranked.head(5), ranked.tail(5)]).drop_duplicates("client")
        print(view[[c for c in ["client", "label", "initial_net_worth", "final_net_worth", "pnl", "pnl_pct", "trades_as_buyer", "trades_as_seller"] if c in view.columns]].to_string(index=False, float_format=lambda v: f"{v:,.2f}"))
    print()


def write_html(data, out: Path, images: list[str], report_dir: Path, suffix: str) -> Path:
    summary = data["summary"]
    rows = "".join(f"<tr><td>{html.escape(str(k))}</td><td>{html.escape(str(v))}</td></tr>" for k, v in zip(summary.get("key", []), summary.get("value", [])))
    symbols_table = data["symbols"].to_html(index=False, float_format=lambda v: f"{v:,.2f}", border=0, classes="t")
    final = data["portfolios_final"]
    portfolios_table = (final.sort_values("pnl", ascending=False).drop(columns=["holdings"], errors="ignore").to_html(index=False, float_format=lambda v: f"{v:,.2f}", border=0, classes="t")) if not final.empty else ""
    imgs = "".join(f'<figure><img src="{html.escape(name)}" alt="{html.escape(name)}"/></figure>' for name in images)
    strategies = strategy_table(final, data.get("portfolio_samples"), data["symbols"])
    strategies_html = ""
    if not strategies.empty:
        fmt = lambda v: f"{v:+,.2f}"
        strategies_html = ("<h2>Strategies that only see prices, ranked</h2><p class=\"n\">By what they added over holding their starting "
                           "account (excess, in %): the raw P&amp;L is mostly the market's own move, every bot holds stock from the start.</p>"
                           f"<div class=\"wrap\">{strategies[~strategies['informed']].drop(columns='informed').to_html(float_format=fmt, border=0, classes='t')}</div>")
        if strategies["informed"].any():
            strategies_html += ("<h2>Informed benchmark, not ranked</h2><p class=\"n\">The value investors see each company's hidden "
                                "fundamental value, the variable that moves the prices, which no other bot sees: what knowing the value is "
                                "worth, not a strategy to beat.</p>"
                                f"<div class=\"wrap\">{strategies[strategies['informed']].drop(columns='informed').to_html(float_format=fmt, border=0, classes='t')}</div>")
    page = f"""<!doctype html><html><head><meta charset="utf-8"><title>Market report</title>
<style>
body {{ background:{BG}; color:{TEXT}; font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif; margin:24px; }}
h1,h2 {{ font-weight:600; }} h2 {{ margin-top:32px; border-bottom:1px solid {GRID}; padding-bottom:6px; }}
table.t, table.s {{ border-collapse:collapse; font-size:13px; }}
table.t th, table.t td, table.s td {{ padding:4px 10px; border-bottom:1px solid {GRID}; text-align:right; }}
table.t th {{ color:{MUTED}; }} table.s td:first-child {{ text-align:left; color:{MUTED}; }}
figure {{ margin:18px 0; }} img {{ max-width:100%; border:1px solid {GRID}; border-radius:6px; }}
.wrap {{ overflow-x:auto; }} .n {{ color:{MUTED}; max-width:900px; }}
</style></head><body>
<h1>Market report: {html.escape(TITLES.get(suffix, suffix))}</h1><p style="color:{MUTED}">source: {html.escape(display_path(report_dir))} (files *_{html.escape(suffix)}.csv)</p>
<h2>Summary</h2><table class="s">{rows}</table>
<h2>Symbols: before vs after</h2><div class="wrap">{symbols_table}</div>
{strategies_html}
<h2>Charts</h2>{imgs}
<h2>Client portfolios</h2><div class="wrap">{portfolios_table}</div>
</body></html>"""
    path = out / f"report_{suffix}.html"
    path.write_text(page, encoding="utf-8")
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report_dir", type=Path, help="run folder holding the report CSVs")
    parser.add_argument("--suffix", choices=KNOWN_SUFFIXES, default=None, help="which report to plot when the folder holds both")
    parser.add_argument("--output-dir", type=Path, default=None, help="where to write the charts (default: REPORT_DIR/plots)")
    parser.add_argument("--candle-seconds", type=float, default=None, help="candle width in seconds (default: run duration / 40)")
    parser.add_argument("--highlight", type=int, default=3, help="how many top/bottom clients to highlight (default 3)")
    parser.add_argument("--show", action="store_true", help="also open the charts in interactive windows")
    args = parser.parse_args()

    if not args.report_dir.is_dir():
        raise SystemExit(f"{args.report_dir} is not a directory")
    suffix = resolve_suffix(args.report_dir, args.suffix)

    headless = not args.show or (sys.platform.startswith("linux") and not os.environ.get("DISPLAY"))
    if headless:
        import matplotlib
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    apply_style(plt)

    data = load_report(args.report_dir, suffix)
    out = args.output_dir or (args.report_dir / "plots")
    out.mkdir(parents=True, exist_ok=True)

    print(f"[{TITLES.get(suffix, suffix)}]")
    print_summary(data)

    charts = [
        ("01_price_paths", lambda: plot_price_paths(plt, data, out)),
        ("02_before_after", lambda: plot_before_after(plt, data, out)),
        ("03_candles", lambda: plot_candles(plt, data, out, args.candle_seconds)),
        ("04_depth", lambda: plot_depth(plt, data, out)),
        ("05_book_pressure", lambda: plot_book_pressure(plt, data, out)),
        ("06_order_lifecycle", lambda: plot_order_lifecycle(plt, data, out)),
        ("07_portfolios", lambda: plot_portfolios(plt, data, out, args.highlight)),
        ("08_trade_flow", lambda: plot_trade_flow(plt, data, out)),
        ("09_waiting_orders", lambda: plot_waiting_orders(plt, data, out)),
        ("10_strategies", lambda: plot_strategies(plt, data, out)),
    ]
    written = []
    metrics_png = out / f"metrics_{suffix}.png" # from plot_metrics.py, if it ran first
    if metrics_png.exists():
        written.append(metrics_png.name)
    for base, make in charts:
        fig = make()
        name = f"{base}_{suffix}.png"
        if fig is None:
            print(f"skipped {name} (no data for it in this report)")
            continue
        fig.savefig(out / name, dpi=140, bbox_inches="tight")
        written.append(name)
        if headless:
            plt.close(fig)

    html_path = write_html(data, out, written, args.report_dir, suffix)
    print(f"wrote {len(written)} charts and {display_path(html_path)}")
    if not headless:
        plt.show()


if __name__ == "__main__":
    main()
