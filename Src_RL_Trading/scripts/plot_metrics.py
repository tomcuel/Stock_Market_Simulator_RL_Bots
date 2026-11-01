#!/usr/bin/env python3
"""
plot_metrics.py: plots the throughput/latency metrics a run recorded over time

Input: a run folder (or directly a metrics CSV) produced by
    ./simulation.x --output-dir DIR      -> DIR/metrics_simu.csv
    ./sim_server.x --output-dir DIR      -> DIR/metrics_bots.csv
    ./launch.sh                          -> Src_Simulation/output/<timestamp>/metrics_{simu,bots}.csv
Output: DIR/plots/metrics_<simu|bots>.png  (three panels: orders/sec + trades/sec, submit latency, rejections + waiting STOP/LIMIT_STOP orders): the launchers call this automatically

Usage (from Src_Simulation/):
    python3 scripts/plot_metrics.py output/latest                   # the newest launcher run
    python3 scripts/plot_metrics.py output/latest --suffix bots      # needed when the folder holds both metrics_simu.csv and metrics_bots.csv
    python3 scripts/plot_metrics.py output/my_run/metrics_simu.csv   # a CSV file directly

Options (all optional):
    --suffix simu|bots   which metrics file to plot when the folder has more than one
    --output PATH        where to save the PNG (default: <run folder>/plots/metrics_<suffix>.png)
    --show               also open the chart in a window (needs a display)

CSV columns (one row per metrics interval, counters are cumulative since the start of the run):
    elapsed_ms, orders_submitted, orders_accepted, orders_rejected, orders_queued, orders_expired, trades_executed, volume_traded, waiting_orders, mean_latency_us, max_latency_us
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import pandas as pd

KNOWN_SUFFIXES = ("simu", "bots")


def display_path(path) -> str:
    """
    A path as shown to the user: relative to the project root (the folder holding Src_Simulation/ and NRT/), e.g. Src_Simulation/output/20260925_012005, never absolute
    """
    resolved = Path(path).resolve()
    for parent in [resolved, *resolved.parents]:
        if (parent / "Src_Simulation").is_dir() and (parent / "NRT").is_dir():
            return str(resolved.relative_to(parent))
    return str(path)


def resolve_csv(path: Path, suffix: str | None) -> tuple[Path, str]:
    """
    Returns (csv file, suffix without underscore) for a run folder or a CSV path
    """
    if path.is_file():
        stem = path.stem  # e.g. metrics_simu
        return path, stem.split("_", 1)[1] if "_" in stem else ""
    if not path.is_dir():
        raise SystemExit(f"{path} is neither a metrics CSV nor a run folder")
    if suffix:
        csv = path / f"metrics_{suffix}.csv"
        if not csv.exists():
            raise SystemExit(f"{csv} not found")
        return csv, suffix
    found = sorted(path.glob("metrics_*.csv"))
    if not found:
        raise SystemExit(f"no metrics_*.csv in {path}: was the run started with --output-dir (and without --no-metrics)?")
    if len(found) > 1:
        names = ", ".join(f.name for f in found)
        raise SystemExit(f"{path} holds several metrics files ({names}): pick one with --suffix {'|'.join(KNOWN_SUFFIXES)}")
    return found[0], found[0].stem.split("_", 1)[1]


def load_metrics(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    if df.empty:
        raise SystemExit(f"{path} has no data rows: the run stopped before its first metrics tick")
    df["elapsed_s"] = df["elapsed_ms"] / 1000.0
    interval_s = df["elapsed_ms"].diff().fillna(df["elapsed_ms"]) / 1000.0
    interval_s = interval_s.where(interval_s > 0)
    df["orders_per_sec"] = (df["orders_submitted"].diff().fillna(df["orders_submitted"]) / interval_s).fillna(0.0)
    df["trades_per_sec"] = (df["trades_executed"].diff().fillna(df["trades_executed"]) / interval_s).fillna(0.0)
    return df


def plot(plt, df: pd.DataFrame, title: str):
    fig, axes = plt.subplots(3, 1, figsize=(11, 10), sharex=True)
    fig.suptitle(title, fontsize=13, fontweight="bold")

    ax = axes[0]
    ax.plot(df["elapsed_s"], df["orders_per_sec"], label="orders/sec", color="#42a5f5")
    ax.plot(df["elapsed_s"], df["trades_per_sec"], label="trades/sec", color="#ffa726")
    ax.set_ylabel("per second")
    ax.set_title("Throughput", loc="left")
    ax.legend()

    ax = axes[1]
    ax.plot(df["elapsed_s"], df["mean_latency_us"], label="mean latency (us)", color="#26a69a")
    ax.plot(df["elapsed_s"], df["max_latency_us"], label="max latency (us)", color="#ef5350", alpha=0.7)
    ax.set_ylabel("microseconds")
    ax.set_title("Order submit latency (cumulative mean / max since start)", loc="left")
    ax.legend()

    ax = axes[2]
    ax.plot(df["elapsed_s"], df["orders_rejected"], label="orders rejected (cumulative)", color="#ef5350")
    ax.plot(df["elapsed_s"], df["waiting_orders"], label="waiting orders (current)", color="#ab47bc")
    ax.set_xlabel("elapsed seconds")
    ax.set_ylabel("count")
    ax.set_title("Rejections and the STOP/LIMIT_STOP waiting registry", loc="left")
    ax.legend()

    for ax in axes:
        ax.grid(True, alpha=0.3)
    fig.tight_layout()
    return fig


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", type=Path, help="run folder (containing metrics_*.csv) or a metrics CSV file")
    parser.add_argument("--suffix", choices=KNOWN_SUFFIXES, default=None, help="which metrics file to use when the folder has several")
    parser.add_argument("--output", type=Path, default=None, help="PNG path (default: <run folder>/plots/metrics_<suffix>.png)")
    parser.add_argument("--show", action="store_true", help="also open the chart interactively")
    args = parser.parse_args()

    csv_path, suffix = resolve_csv(args.path, args.suffix)
    output = args.output or (csv_path.parent / "plots" / f"{csv_path.stem}.png")
    output.parent.mkdir(parents=True, exist_ok=True)

    headless = not args.show or (sys.platform.startswith("linux") and not os.environ.get("DISPLAY"))
    if headless:
        import matplotlib
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    df = load_metrics(csv_path)
    last = df.iloc[-1]
    print(f"{csv_path.name}: {len(df)} samples over {last['elapsed_s']:.1f}s, {int(last['orders_submitted'])} orders, {int(last['trades_executed'])} trades, mean latency {last['mean_latency_us']:.1f}us, max {int(last['max_latency_us'])}us")

    label = {"simu": "simulation (in-process)", "bots": "server + socket bots"}.get(suffix, suffix)
    fig = plot(plt, df, f"Metrics: {label}")
    fig.savefig(output, dpi=140)
    print(f"saved {display_path(output)}")
    if not headless:
        plt.show()


if __name__ == "__main__":
    main()
