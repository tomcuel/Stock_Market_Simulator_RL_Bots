#!/usr/bin/env python3
"""
market_calibration.py: is a recorded market still the calibrated one? The daily statistics of README section 2 (the median over each market's symbols, then the median over its markets), over all its days and by third of its days, next to a baseline market.

Input: a recorded market (scripts/simulate_market_days.sh), RL_Agents/sim_days/<market>/seed_<S>/daily_bars_simu.csv
Usage (from Src_RL_Trading/):
    python3 scripts/market_calibration.py RL_Agents/sim_days/<new> --baseline RL_Agents/sim_days/real5_d600
    python3 scripts/market_calibration.py RL_Agents/sim_days/real5_d600            # the table and the checks over time only

Checks (exit status 1 if one fails):
    against the baseline   volatility, range, overnight gap and spread within --tolerance (default 15%)
    over time              the book's size (trades per day, value per trade, value traded per day) in the last third of the days within --tolerance of the first third
                           a long market must not thin out under the RL bots, which are sized by value on one typical trade (shares per trade is reported, not checked: it moves with the price)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "RL_Agents"))
from rl_agents.daily import CALIBRATION, CALIBRATION_CHECKED, market_statistics  # noqa: E402

BOOK = ["trades_per_day", "value_per_trade_k", "traded_value_m"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("market", type=Path)
    parser.add_argument("--baseline", type=Path, default=None)
    parser.add_argument("--tolerance", type=float, default=0.15)
    args = parser.parse_args()

    whole = market_statistics(args.market)
    columns = {f"{args.market.name} ({len(whole)} markets)": whole.median()}
    thirds = {name: market_statistics(args.market, part).median()
              for name, part in (("first third", (0, 1 / 3)), ("middle third", (1 / 3, 2 / 3)), ("last third", (2 / 3, 1)))}
    columns.update(thirds)
    base = None
    if args.baseline is not None:
        base_rows = market_statistics(args.baseline)
        base = base_rows.median()
        columns[f"{args.baseline.name} ({len(base_rows)} markets)"] = base
    pd.set_option("display.width", 200)
    print(pd.DataFrame(columns).loc[CALIBRATION + ["day_clock_ms", "day_clock_max_ms"]].round(3).to_string())

    failed = 0
    def check(ok: bool, text: str) -> None:
        nonlocal failed
        failed += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {text}")
    print("checks:")
    m = whole.median()
    if base is not None:
        for key in CALIBRATION_CHECKED:
            change = m[key] / base[key] - 1
            check(abs(change) <= args.tolerance, f"{key} {m[key]:.1f} against {base[key]:.1f} in {args.baseline.name}: {100 * change:+.1f}%")
    for key in BOOK:
        change = thirds["last third"][key] / thirds["first third"][key] - 1
        check(change >= -args.tolerance, f"{key} last third against first third: {100 * change:+.1f}%")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
