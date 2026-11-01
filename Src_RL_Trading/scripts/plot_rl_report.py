#!/usr/bin/env python3
"""
plot_rl_report.py: one page for an RL agent's verdict, offline and live: the walk-forward folds, the sealed markets, the out-of-sample curves 
(RL_Agents/reports/, written by python -m rl_agents.train) and the live evaluation (output/rl_eval_<timestamp>/, written by scripts/evaluate_rl_live.sh), with a link to every live market's own report

Output, in the evaluation folder:
    plots/rl_folds.png         per test window: the closed trades' mean R and profit factor, against the gate's lines
    plots/rl_oos_equity.png    the out-of-sample curve of every development market (each fold's test window compounded)
    plots/rl_live_excess.png   live, per strategy: the excess over holding the starting account, one dot per market
    index.html                 the tables (gate, folds, sealed markets, live) and the charts

Usage (from Src_RL_Trading/):
    python3 scripts/plot_rl_report.py output/rl_eval_20261029_220234
    python3 scripts/plot_rl_report.py output/latest --reports RL_Agents/reports --name rl
Run plot_market_report.py on each output/rl_eval_*/seed_*/ first for the per-market links (the page says when one is missing).
"""
from __future__ import annotations

import argparse
import html
import json
import os
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

# the market report's dark theme (plot_market_report.py), series colors validated on its panel
BG, PANEL, GRID, TEXT, MUTED = "#131722", "#1e222d", "#2a2e39", "#d1d4dc", "#787b86"
RL, RULE = "#3987e5", "#d95926"
# the value investors see each company's hidden fundamental value (fundamentals.hpp), which no other strategy sees: an informed benchmark, drawn apart in a neutral gray and never ranked with the strategies that only see prices
INFORMED, INFORMED_COLOR = {"value"}, "#9598a1"


def style(ax) -> None:
    ax.set_facecolor(PANEL)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ax.spines.values():
        side.set_visible(False)
    ax.tick_params(colors=TEXT, labelsize=9)
    ax.xaxis.label.set_color(TEXT)
    ax.yaxis.label.set_color(TEXT)


def figure(rows: int, height: float):
    fig, axes = plt.subplots(rows, 1, figsize=(10, height), facecolor=BG, squeeze=False)
    for ax in axes[:, 0]:
        style(ax)
    return fig, axes[:, 0]


def reference_line(ax, y: float, text: str) -> None:
    ax.axhline(y, color=MUTED, linewidth=1, linestyle=(0, (4, 3)))
    ax.annotate(text, xy=(1, y), xycoords=("axes fraction", "data"), xytext=(-4, 3), textcoords="offset points", ha="right", va="bottom", color=TEXT, fontsize=8)


def plot_folds(folds: pd.DataFrame, path: Path) -> None:
    fig, (top, bottom) = figure(2, 6.4)
    x = folds["fold"].to_numpy()
    r = folds["test_mean_r"].to_numpy()
    top.bar(x, r, width=0.8, color=RL, edgecolor=PANEL, linewidth=2)
    top.axhline(0, color=TEXT, linewidth=1)
    top.set_ylabel("closed trades' mean R")
    top.set_title("Test windows: the agent's closed trades", color=TEXT, fontsize=12, loc="left")
    pf = folds["test_profit_factor"].to_numpy()
    bottom.bar(x, pf, width=0.8, color=RL, edgecolor=PANEL, linewidth=2)
    reference_line(bottom, 1.0, "profit factor 1: a consistent fold needs more (and a positive return)")
    reference_line(bottom, 0.9, "the gate's worst fold: 0.9")
    bottom.set_ylabel("profit factor")
    bottom.set_xlabel("fold (each one: 180 days of training, 30 of validation, 30 of test)")
    for ax in (top, bottom):
        ax.set_xticks(x)
    fig.tight_layout()
    fig.savefig(path, dpi=130, facecolor=BG, bbox_inches="tight", pad_inches=0.2)
    plt.close(fig)


def plot_oos(oos: pd.DataFrame, path: Path) -> None:
    fig, (ax,) = figure(1, 4.2)
    bp = (oos.drop(columns="test_day") - 1.0) * 1e4
    for col in bp.columns:
        ax.plot(oos["test_day"], bp[col], color=MUTED, linewidth=0.8, alpha=0.45)
    ax.plot(oos["test_day"], bp.median(axis=1), color=RL, linewidth=2, label="median over the markets")
    ax.axhline(0, color=TEXT, linewidth=1)
    ax.set_ylabel("bp of the equity (trades still open marked at the close)")
    ax.set_xlabel("out-of-sample day (the test windows of every fold, one after the other)")
    ax.set_title(f"Out-of-sample curve of each of the {bp.shape[1]} development markets", color=TEXT, fontsize=12, loc="left")
    legend = ax.legend(facecolor=PANEL, edgecolor=GRID, fontsize=9, loc="upper left")
    for t in legend.get_texts():
        t.set_color(TEXT)
    fig.tight_layout()
    fig.savefig(path, dpi=130, facecolor=BG, bbox_inches="tight", pad_inches=0.2)
    plt.close(fig)


def live_table(live: pd.DataFrame) -> pd.DataFrame:
    table = live.groupby("strategy")["excess"].agg(["mean", "min", "max", lambda s: int((s > 0).sum()), "size"])
    table.columns = ["mean", "min", "max", "positive", "markets"]
    return table


def bars(axis, rows: pd.DataFrame, live: pd.DataFrame, colors: list) -> None:
    y = np.arange(len(rows))
    axis.barh(y, rows["mean"] / 1e3, height=0.7, color=colors, edgecolor=PANEL, linewidth=2)
    for i, name in enumerate(rows.index):
        values = live.loc[live["strategy"] == name, "excess"] / 1e3
        axis.scatter(values, np.full(len(values), i), s=22, color=TEXT, edgecolor=PANEL, linewidth=1, zorder=3)
    axis.axvline(0, color=TEXT, linewidth=1)
    axis.set_yticks(y)
    axis.set_yticklabels(rows.index)


def plot_live(live: pd.DataFrame, rl_names: set, path: Path) -> tuple[pd.DataFrame, pd.DataFrame]:
    """
    Three panels: the strategies that only see prices, 
    - ranked (RL agents and rule-based bots)
    - the informed benchmark apart, on the same scale (the value investors see the hidden fundamental value), the RL agents on their own scale
    """
    table = live_table(live)
    ranked = table[~table.index.isin(INFORMED)].sort_values("mean")
    informed = table[table.index.isin(INFORMED)]
    zoom = ranked[ranked.index.isin(rl_names)]
    heights = [len(ranked), max(len(informed), 1), max(len(zoom), 1)]
    fig = plt.figure(figsize=(10, 0.42 * sum(heights) + 3.4), facecolor=BG)
    grid = fig.add_gridspec(3, 1, height_ratios=heights, hspace=0.55)
    ax = fig.add_subplot(grid[0])
    ax_informed = fig.add_subplot(grid[1], sharex=ax)
    ax_zoom = fig.add_subplot(grid[2])
    for axis in (ax, ax_informed, ax_zoom):
        style(axis)
    bars(ax, ranked, live, [RL if n in rl_names else RULE for n in ranked.index])
    bars(ax_informed, informed, live, [INFORMED_COLOR] * len(informed))
    bars(ax_zoom, zoom, live, [RL] * len(zoom))
    ax.set_title("Live, on markets no agent saw: what each strategy that only sees prices added, ranked", color=TEXT, fontsize=12, loc="left")
    ax_informed.set_title("informed benchmark, not ranked: the value investors see each company's hidden fundamental value", color=TEXT, fontsize=10, loc="left")
    ax_zoom.set_title("the RL agents and their baselines alone, on their own scale", color=TEXT, fontsize=10, loc="left")
    ax_zoom.set_xlabel("excess over holding the starting account, thousands (10 million per bot); dots: each market")
    from matplotlib.patches import Patch
    legend = ax.legend(handles=[Patch(color=RL, label="RL agents and their baselines"), Patch(color=RULE, label="rule-based bots"), Patch(color=INFORMED_COLOR, label="informed benchmark (value investors)")], facecolor=PANEL, edgecolor=GRID, fontsize=9, loc="upper left")
    for t in legend.get_texts():
        t.set_color(TEXT)
    fig.savefig(path, dpi=130, facecolor=BG, bbox_inches="tight", pad_inches=0.2)
    plt.close(fig)
    return ranked.sort_values("mean", ascending=False), informed


def html_table(frame: pd.DataFrame) -> str:
    return frame.to_html(escape=True, border=0, classes="t", na_rep="")


def live_detail(out: Path, reports: Path, name: str, live: pd.DataFrame, rl_names: set, plots_dir: Path) -> str:
    """
    The live charts of rl_agents.plots (the RL bots over the days, their trades on the candles, the execution against the environment's assumptions, how much each agent trades) and the links to the training and calibration pages, as HTML
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "RL_Agents"))
    from rl_agents import plots as rl_plots
    plt_ = rl_plots.plotting()
    seeds = sorted(out.glob("seed_*"), key=lambda p: int(p.name.split("_")[1]))
    if plt_ is None or not seeds:
        return ""
    read = lambda folder, f: pd.read_csv(folder / f) if (folder / f).exists() else pd.DataFrame()
    days, costs = {}, ([], [], [])
    for folder in seeds:
        seed = int(folder.name.split("_")[1])
        daily, trades = read(folder, "daily_bars_simu.csv"), read(folder, "rl_trades_simu.csv")
        if daily.empty:
            continue
        days[seed] = int(daily["day"].max()) + 1
        if not trades.empty:
            for bucket, values in zip(costs, rl_plots.execution_costs(trades, daily)):
                bucket.append(values)
    first = seeds[0]
    daily, trades = read(first, "daily_bars_simu.csv"), read(first, "rl_trades_simu.csv")
    samples, prices, final = read(first, "portfolio_samples_simu.csv"), read(first, "price_samples_simu.csv"), read(first, "portfolios_final_simu.csv")
    images = []
    if not daily.empty and not samples.empty and not final.empty:
        rl_plots.plot_rl_equity(plt_, samples, prices, final, daily, rl_names, plots_dir / "rl_bots_over_days.png")
        images.append(("rl_bots_over_days.png", f"How did each RL bot's account move, day after day ({first.name})? Its excess over holding its starting account, the market above."))
    if not daily.empty and not trades.empty:
        rl_plots.plot_rl_trades_on_candles(plt_, daily, trades, plots_dir / "rl_trades_on_candles.png")
        images.append(("rl_trades_on_candles.png", f"Where did they trade ({first.name}, the symbol with the most RL trades)? Every entry, its stop and target, its exit by reason."))
    if costs[0]: # at least one market with RL trades
        meta_path = reports.parent / "artifacts" / name / "metadata.json"
        env = json.loads(meta_path.read_text()).get("env", {}) if meta_path.exists() else {}
        spread = pd.concat([((d["ask"] - d["bid"]) / ((d["ask"] + d["bid"]) / 2) * 1e4).where((d["bid"] > 0) & (d["ask"] > 0)) for d in (read(f, "daily_bars_simu.csv") for f in seeds) if not d.empty])
        assumed_entry = 0.5 * float(spread.median()) + float(env.get("slippage_bps", 5.0))
        rl_plots.plot_execution(plt_, tuple(pd.concat(c) if c else pd.Series(dtype=float) for c in costs), plots_dir / "rl_execution.png", float(env.get("stop_slippage_bps", 100.0)), assumed_entry)
        images.append(("rl_execution.png", f"Does live match training? Every RL fill of the {len(seeds)} markets against the environment's assumptions: an entry costs half the spread before the close plus {env.get('slippage_bps', 5):g} bp ({assumed_entry:.0f} bp here), a stop-loss {env.get('stop_slippage_bps', 100):g} bp past the stop, a take-profit its target."))
    if days:
        rl_plots.plot_activity(plt_, live[live["strategy"].isin(rl_names)], days, plots_dir / "rl_activity.png")
        images.append(("rl_activity.png", "How much does each agent trade? Its entries per trading day per bot (orders per intraday decision would need the intraday bot, not built)."))
    links = []
    training = reports / name / "plots" / "index.html"
    if training.exists():
        links.append(f'<a href="{html.escape(os.path.relpath(training, out))}">the training charts</a> (learning curves, PPO diagnostics, folds, out of sample, actions, sealed markets)')
    summary_path = reports / f"{name}_summary.json"
    source = json.loads(summary_path.read_text()).get("provenance", {}).get("source", "") if summary_path.exists() else ""
    calibration = reports / Path(source).name / "plots" / "index.html" if source else None
    if calibration is not None and calibration.exists():
        links.append(f'<a href="{html.escape(os.path.relpath(calibration, out))}">the calibration charts</a> of the market it trained in')
    body = "".join(f'<p class="note">{html.escape(note)}</p><img src="plots/{name_}"/>' for name_, note in images)
    return ("<h2>The RL bots, live</h2>" + body + (f"<h2>More</h2><ul>{''.join(f'<li>{l}</li>' for l in links)}</ul>" if links else ""))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("evaluation", type=Path, help="an output/rl_eval_<timestamp>/ folder")
    parser.add_argument("--reports", type=Path, default=Path(__file__).resolve().parent.parent / "RL_Agents" / "reports")
    parser.add_argument("--name", default="rl", help="the agent's name in the reports (reports/<name>_*)")
    args = parser.parse_args()
    out = args.evaluation.resolve()
    plots = out / "plots"
    plots.mkdir(parents=True, exist_ok=True)

    summary = json.loads((args.reports / f"{args.name}_summary.json").read_text())
    folds = pd.read_csv(args.reports / f"{args.name}_folds.csv")
    oos = pd.read_csv(args.reports / f"{args.name}_oos_equity.csv")
    live = pd.read_csv(out / "summary.csv")
    rl_names = {s for s in live["strategy"].unique() if s == args.name or s.startswith(args.name + "_")}

    plot_folds(folds, plots / "rl_folds.png")
    plot_oos(oos, plots / "rl_oos_equity.png")
    ranked_table, informed_table = plot_live(live, rl_names, plots / "rl_live_excess.png")
    live_charts = live_detail(out, args.reports, args.name, live, rl_names, plots)

    fold_view = pd.DataFrame({
        "fold": folds["fold"], "test days": folds["test"], "return (bp)": (100 * folds["test_return_pct"]).round(2),
        "Sharpe": folds["test_sharpe"].round(2), "profit factor": folds["test_profit_factor"].round(2),
        "closed trades": folds["test_trades"], "win rate": (100 * folds["test_win_rate"]).round(0).astype(int).astype(str) + "%",
        "mean R": folds["test_mean_r"].round(3), "always long (bp)": (100 * folds["always_long_return_pct"]).round(2),
        "trend-hold (bp)": (100 * folds["trend_hold_return_pct"]).round(2)}).set_index("fold")
    sealed = pd.DataFrame({name: {"return (bp)": round(100 * m["return_pct"], 2), "Sharpe": round(m["sharpe"], 2),
                                  "profit factor": round(m["profit_factor"], 2), "trades": m["trades"]}
                           for name, m in summary["holdout"].items() if isinstance(m, dict)}).T.astype({"trades": int})
    rl_live = live.dropna(subset=["mean_r"]).groupby("strategy").agg(trades=("closed", "sum"), mean_r=("mean_r", "mean"), win_rate=("win_rate", "mean"))

    def view(table: pd.DataFrame, ranked: bool) -> pd.DataFrame:
        v = (table[["mean", "min", "max", "positive", "markets"]] / [1e3, 1e3, 1e3, 1, 1]).round(1)
        v.columns = ["mean excess (k)", "worst market (k)", "best market (k)", "markets above holding", "markets"]
        v = v.astype({"markets above holding": int, "markets": int}).join(rl_live.round(3), how="left")
        v["trades"] = v["trades"].astype("Int64")
        if ranked:
            v.insert(0, "rank", range(1, len(v) + 1))
        return v
    live_view, informed_view = view(ranked_table, True), view(informed_table, False)
    provenance = summary.get("provenance", {})

    markets = []
    for seed_dir in sorted(out.glob("seed_*")):
        report = seed_dir / "plots" / "report_simu.html"
        link = f'<a href="{seed_dir.name}/plots/report_simu.html">{seed_dir.name}</a>' if report.exists() else f"{seed_dir.name} (no report: plot_market_report.py)"
        markets.append(f"<li>{link}</li>")
    gate = "".join(f"<li><code>{html.escape(line)}</code></li>" for line in summary["gate"])
    page = f"""<!doctype html><html><head><meta charset="utf-8"><title>RL agent verdict</title>
<style>body{{background:{BG};color:{TEXT};font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;margin:24px;max-width:1100px}}
a{{color:#42a5f5}}h2{{border-bottom:1px solid {GRID};padding-bottom:6px;margin-top:32px}}img{{max-width:100%;border:1px solid {GRID};border-radius:6px}}
table.t{{border-collapse:collapse;font-size:13px;margin:8px 0}}table.t th,table.t td{{padding:4px 10px;border-bottom:1px solid {GRID};text-align:right}}
table.t th{{color:{TEXT}}}code{{font-size:13px}}.note{{color:#b2b5be;max-width:900px;line-height:1.45}}</style></head><body>
<h1>RL agent <code>{html.escape(args.name)}</code>: the verdict</h1>
<p class="note">Trained on {html.escape(str(provenance.get('source', '?')))}: {len(provenance.get('seeds', []))} development markets, sealed
{html.escape(str(provenance.get('sealed_seeds', [])))}; judged live in <code>{html.escape(out.name)}</code>.</p>
<h2>The gate: {'PASSED' if summary['gate_passed'] else 'FAILED'}</h2><ul>{gate}</ul>
<p class="note">A fold's return is its equity curve, which marks the trades still open at the end of the window at the last close;
its profit factor and mean R count the closed trades only. Where the two disagree (a positive return with a profit factor below 1),
the return came from trades still open, not from the agent's closed trades.</p>
<h2>Walk-forward test windows</h2><img src="plots/rl_folds.png"/>{html_table(fold_view)}
<img src="plots/rl_oos_equity.png"/>
<h2>Sealed markets, evaluated once</h2>{html_table(sealed)}
<p class="note">Buy and hold on the sealed markets: {summary['holdout'].get('buy_and_hold_pct', float('nan')):+.2f}%.</p>
<h2>Live, inside the exchange</h2><img src="plots/rl_live_excess.png"/>
<h3>Strategies that only see prices, ranked</h3>{html_table(live_view)}
<h3>Informed benchmark, not ranked</h3>{html_table(informed_view) if len(informed_view) else '<p class="note">(no value investors in these markets)</p>'}
<p class="note">The value investors see each company's hidden fundamental value, the random walk that moves the prices, which no
other bot and no agent sees: they trade on the news before it is in the price. Their excess is what knowing the value is worth,
an upper bound, not a strategy the RL agent could match from prices alone.</p>
<p class="note">The excess is what a strategy added beyond holding its starting account (half in stock): the raw P&amp;L of each
market's report is mostly the market's own rise.</p>
{live_charts}
<h2>Each live market's full report</h2><ul>{''.join(markets)}</ul>
</body></html>"""
    (out / "index.html").write_text(page)
    print(f"wrote {out / 'index.html'} and its charts in {plots}")


if __name__ == "__main__":
    main()
