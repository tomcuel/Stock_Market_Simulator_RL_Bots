"""
The charts of every stage, written automatically at the end of its run, as PNGs plus one HTML page linking them. 
Each chart answers one question, said in its title

    calibration   reports/<market>/plots/      rl_agents.calibration, after each recording (scripts/simulate_market_days.sh)
    training      reports/<name>/plots/        rl_agents.train, at its end
    live          output/rl_eval_<timestamp>/  scripts/plot_rl_report.py, called by scripts/evaluate_rl_live.sh

Regenerate a stage's page from the files it already wrote:
    python -m rl_agents.plots training --name rl                  # from reports/rl_*
"""
from __future__ import annotations

import argparse
import html
import json
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent

# the market report's dark theme; series colors validated on its panel (dataviz validator, dark mode, surface #1e222d)
BG, PANEL, GRID, TEXT, MUTED = "#131722", "#1e222d", "#2a2e39", "#d1d4dc", "#787b86"
AGENT, ALWAYS_LONG, TREND_HOLD, RANDOM = "#3987e5", "#d95926", "#199e70", "#c98500"
SIMULATED, REAL = AGENT, ALWAYS_LONG
UP, DOWN, NEUTRAL = "#26a69a", "#ef5350", "#9598a1"
BASELINE_COLORS = {"agent": AGENT, "always_long": ALWAYS_LONG, "trend_hold": TREND_HOLD, "random": RANDOM, "flat": NEUTRAL, "ensemble": "#9085e9"}
EXIT_COLORS = {"TP": UP, "SL": DOWN, "close": NEUTRAL, "flip": "#9085e9", "both": RANDOM}


def plotting():
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed: charts skipped (pip install matplotlib)")
        return None
    plt.rcParams.update({"figure.facecolor": BG, "axes.facecolor": PANEL, "axes.edgecolor": GRID, "axes.labelcolor": TEXT,
                         "axes.titlecolor": TEXT, "xtick.color": TEXT, "ytick.color": TEXT, "grid.color": GRID, "text.color": TEXT,
                         "axes.grid": True, "axes.axisbelow": True, "legend.facecolor": PANEL, "legend.edgecolor": GRID,
                         "font.size": 9, "axes.titlesize": 10, "axes.titlelocation": "left"})
    return plt


def save(fig, path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=120, facecolor=BG, bbox_inches="tight", pad_inches=0.2)
    import matplotlib.pyplot as plt
    plt.close(fig)
    return path


def reference(ax, y: float, text: str, horizontal: bool = True, left: bool = False) -> None:
    if horizontal:
        ax.axhline(y, color=MUTED, linewidth=1, linestyle=(0, (4, 3)))
        ax.annotate(text, xy=(0 if left else 1, y), xycoords=("axes fraction", "data"), xytext=(4 if left else -4, 3), textcoords="offset points", ha="left" if left else "right", va="bottom", fontsize=8)
    else:
        ax.axvline(y, color=MUTED, linewidth=1, linestyle=(0, (4, 3)))
        ax.annotate(text, xy=(y, 1), xycoords=("data", "axes fraction"), xytext=(3, -12), textcoords="offset points", ha="left", fontsize=8)


def index_page(folder: Path, title: str, intro: str, sections: list[tuple[str, list[str], str]]) -> Path:
    """
    One HTML page: every section a heading, its charts (file names in folder) and a note
    """
    body = []
    for heading, images, note in sections:
        body.append(f"<h2>{html.escape(heading)}</h2>")
        if note:
            body.append(f'<p class="note">{note}</p>')
        body.extend(f'<img src="{html.escape(name)}" alt="{html.escape(name)}"/>' for name in images if (folder / name).exists())
    page = f"""<!doctype html><html><head><meta charset="utf-8"><title>{html.escape(title)}</title>
<style>body{{background:{BG};color:{TEXT};font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;margin:24px;max-width:1150px}}
a{{color:#42a5f5}}h2{{border-bottom:1px solid {GRID};padding-bottom:6px;margin-top:32px}}img{{max-width:100%;border:1px solid {GRID};border-radius:6px;margin:8px 0}}
.note{{color:#b2b5be;max-width:950px;line-height:1.45}}</style></head><body><h1>{html.escape(title)}</h1><p class="note">{intro}</p>
{''.join(body)}</body></html>"""
    path = folder / "index.html"
    path.write_text(page)
    return path


# =============================================================================================== calibration
CALIBRATION_PANELS = [("drift_bp", "drift (bp a day)"), ("vol_bp", "volatility (bp a day)"), ("autocorr", "lag-1 autocorrelation"),
                      ("range_bp", "range, high - low (bp of the close)"), ("abs_gap_bp", "overnight gap, mean |gap| (bp)"),
                      ("overnight_share", "share of the variance overnight")]


def plot_calibration_stats(plt, simulated: pd.DataFrame, real: pd.DataFrame, path: Path) -> Path:
    """
    Is the simulated market a stock market? Each statistic: 
    one dot per simulated market (median over its symbols), the real tickers' range shaded, each real ticker a dot
    """
    fig, axes = plt.subplots(2, 3, figsize=(13, 6.5))
    fig.suptitle("The simulated market against the real tickers it starts from: one dot per simulated market, the real range shaded", fontsize=12, fontweight="bold", color=TEXT)
    rng = np.random.default_rng(0)
    for ax, (key, label) in zip(axes.flat, CALIBRATION_PANELS):
        sim = simulated[key].dropna().to_numpy() if key in simulated else np.array([])
        rea = real[key].dropna().to_numpy() if key in real else np.array([])
        if len(rea):
            ax.axhspan(rea.min(), rea.max(), color=REAL, alpha=0.18, linewidth=0)
            ax.scatter(np.full(len(rea), 1.0) + rng.uniform(-0.08, 0.08, len(rea)), rea, s=36, color=REAL, edgecolor=PANEL, zorder=3, label="real tickers")
        if len(sim):
            ax.scatter(rng.uniform(-0.15, 0.15, len(sim)), sim, s=14, color=SIMULATED, edgecolor=PANEL, linewidth=0.5, zorder=3, label="simulated markets")
            ax.hlines(np.median(sim), -0.3, 0.3, color=TEXT, linewidth=1.5)
        ax.set_xticks([0, 1])
        ax.set_xticklabels(["simulated", "real"])
        ax.set_xlim(-0.6, 1.6)
        ax.set_title(label)
    axes.flat[0].legend(fontsize=8, loc="best")
    fig.tight_layout()
    return save(fig, path)


def plot_return_distributions(plt, sim_returns: np.ndarray, sim_gaps: np.ndarray, real_returns: np.ndarray, real_gaps: np.ndarray, path: Path) -> Path:
    """
    Do the simulated days move like real ones? The distribution of the daily log returns (close to close) and of the overnight gaps, as densities, simulated and real on the same bins
    """
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.4))
    fig.suptitle("Daily returns and overnight gaps: simulated against real (densities, same bins)", fontsize=12, fontweight="bold", color=TEXT)
    for ax, sim, rea, label in ((axes[0], sim_returns, real_returns, "daily return, close to close (bp)"), (axes[1], sim_gaps, real_gaps, "overnight gap, open against the previous close (bp)")):
        sim, rea = 1e4 * np.asarray(sim, dtype=float), 1e4 * np.asarray(rea, dtype=float)
        both = np.concatenate([sim, rea]) if len(rea) else sim
        lo, hi = np.percentile(both, [0.5, 99.5]) if len(both) else (-1, 1)
        bins = np.linspace(lo, hi, 61)
        for values, color, name in ((sim, SIMULATED, "simulated"), (rea, REAL, "real")):
            if len(values):
                ax.hist(values, bins=bins, density=True, histtype="step", linewidth=2, color=color, label=f"{name}: std {values.std():.0f} bp, {len(values):,} days")
        ax.set_xlabel(label)
        ax.set_ylabel("density")
        ax.legend(fontsize=8)
    fig.tight_layout()
    return save(fig, path)


def autocorrelations(paths: list[np.ndarray], max_lag: int) -> np.ndarray:
    out = np.full((len(paths), max_lag), np.nan)
    for i, r in enumerate(paths):
        r = np.asarray(r, dtype=float)
        r = r[np.isfinite(r)]
        for lag in range(1, max_lag + 1):
            if len(r) > lag + 2:
                out[i, lag - 1] = np.corrcoef(r[:-lag], r[lag:])[0, 1]
    return out


def plot_autocorrelation(plt, sim_paths: list[np.ndarray], real_paths: list[np.ndarray], path: Path, max_lag: int = 10) -> Path:
    """
    Can yesterday's move predict today's? The autocorrelation of the daily returns by lag: 
    the simulated paths' median and 10-90% band, each real ticker a dot; the noise band of a random walk shaded
    """
    fig, ax = plt.subplots(figsize=(11, 4.2))
    lags = np.arange(1, max_lag + 1)
    sim = autocorrelations(sim_paths, max_lag)
    n = np.median([len(p) for p in sim_paths]) if sim_paths else 250
    ax.axhspan(-1.96 / np.sqrt(n), 1.96 / np.sqrt(n), color=MUTED, alpha=0.15, linewidth=0, label=f"95% band of a random walk ({int(n)} days)")
    if len(sim):
        ax.fill_between(lags, np.nanpercentile(sim, 10, axis=0), np.nanpercentile(sim, 90, axis=0), color=SIMULATED, alpha=0.25, linewidth=0)
        ax.plot(lags, np.nanmedian(sim, axis=0), color=SIMULATED, linewidth=2, marker="o", label=f"simulated: median of {len(sim)} paths, 10-90% band")
    real = autocorrelations(real_paths, max_lag)
    for i, row in enumerate(real):
        ax.scatter(lags + 0.12, row, s=26, color=REAL, edgecolor=PANEL, zorder=3, label="real tickers" if i == 0 else None)
    ax.axhline(0, color=TEXT, linewidth=0.8)
    ax.set_xticks(lags)
    ax.set_xlabel("lag (trading days)")
    ax.set_ylabel("autocorrelation of the daily returns")
    ax.set_title("Can yesterday's move predict today's? The autocorrelation of the daily returns, by lag")
    ax.legend(fontsize=8)
    fig.tight_layout()
    return save(fig, path)


# =============================================================================================== training
def leg_scores(evals: pd.DataFrame, dd_penalty: float = 0.5) -> pd.DataFrame:
    """
    Each checkpoint's two legs in bp (return - dd_penalty x max drawdown, the train.py score) and the checkpoint's score
    """
    e = evals.copy()
    e["train_bp"] = 100 * (e["train_eval_ret"] - dd_penalty * e["train_dd"])
    e["val_bp"] = 100 * (e["val_ret"] - dd_penalty * e["val_dd"])
    e["score_bp"] = 100 * e["score"]
    return e


def chosen_seeds(evals: pd.DataFrame, folds: pd.DataFrame) -> dict[int, int]:
    """
    The PPO seed whose checkpoint each fold kept: the one whose best eligible score is the fold's validation score
    """
    chosen = {}
    for fold, g in evals.groupby("fold"):
        target = folds.loc[folds["fold"] == fold, "val_score"]
        eligible = g[g["eligible"].astype(bool)]
        if len(target) and pd.notna(target.iloc[0]) and len(eligible):
            row = eligible.iloc[(eligible["score"] - float(target.iloc[0])).abs().argmin()]
            chosen[int(fold)] = int(row["seed"])
        else:
            chosen[int(fold)] = int(g["seed"].min())
    return chosen


def plot_learning_curves(plt, evals: pd.DataFrame, folds: pd.DataFrame, path: Path) -> Path:
    """
    Did training find a checkpoint good on both legs? Per fold (the kept PPO seed): 
    the train-eval and validation scores at each checkpoint, eligible checkpoints filled, the kept one starred
    """
    e = leg_scores(evals)
    chosen = chosen_seeds(evals, folds)
    fold_ids = sorted(e["fold"].unique())
    cols = 4 if len(fold_ids) > 6 else min(3, len(fold_ids))
    rows = int(np.ceil(len(fold_ids) / cols))
    fig, axes = plt.subplots(rows, cols, figsize=(4 * cols, 2.9 * rows), sharex=True, squeeze=False)
    fig.suptitle("Learning curves: each checkpoint's train-eval and validation scores (bp: return - 0.5 x drawdown), eligible ones filled, the kept one starred", fontsize=11, fontweight="bold", color=TEXT)
    for ax, fold in zip(axes.flat, fold_ids):
        g = e[(e["fold"] == fold) & (e["seed"] == chosen.get(int(fold), 1))].sort_values("timesteps")
        x = g["timesteps"] / 1e3
        ax.plot(x, g["train_bp"], color=ALWAYS_LONG, linewidth=1.3, label="train-eval leg")
        ax.plot(x, g["val_bp"], color=AGENT, linewidth=1.3, label="validation leg")
        ok = g["eligible"].astype(bool)
        ax.scatter(x[ok], g.loc[ok, "val_bp"], s=18, color=AGENT, zorder=3)
        ax.scatter(x[~ok], g.loc[~ok, "val_bp"], s=18, facecolor=PANEL, edgecolor=AGENT, zorder=3)
        target = folds.loc[folds["fold"] == fold, "val_score"]
        if len(target) and pd.notna(target.iloc[0]):
            kept = g.iloc[(g["score"] - float(target.iloc[0])).abs().argmin()]
            ax.scatter([kept["timesteps"] / 1e3], [kept["val_bp"]], marker="*", s=160, color=TEXT, edgecolor=PANEL, zorder=4)
        ax.axhline(0, color=MUTED, linewidth=0.8)
        ax.set_title(f"fold {fold} (seed {chosen.get(int(fold), 1)}): {int(ok.sum())} of {len(g)} eligible")
    for ax in axes.flat[len(fold_ids):]:
        ax.set_visible(False)
    for ax in axes[-1]:
        ax.set_xlabel("PPO steps (thousands)")
    axes.flat[0].legend(fontsize=7, loc="best")
    fig.tight_layout()
    return save(fig, path)


def plot_ppo_diagnostics(plt, evals: pd.DataFrame, path: Path, max_entropy: float | None = None) -> Path:
    """
    Is PPO converging or still random? Entropy, approximate KL and explained variance at each checkpoint: every (fold, seed) a thin line, their median in blue
    """
    fig, axes = plt.subplots(1, 3, figsize=(14, 4))
    fig.suptitle("PPO diagnostics: every (fold, seed) training a grey line, the median in blue", fontsize=11, fontweight="bold", color=TEXT)
    for ax, key, label in ((axes[0], "entropy", "policy entropy (nats)"), (axes[1], "approx_kl", "approximate KL per update"),
                           (axes[2], "explained_variance", "value function: explained variance")):
        for _, g in evals.groupby(["fold", "seed"]):
            g = g.sort_values("timesteps")
            ax.plot(g["timesteps"] / 1e3, g[key], color=MUTED, linewidth=0.7, alpha=0.5)
        med = evals.groupby("timesteps")[key].median()
        ax.plot(med.index / 1e3, med.values, color=AGENT, linewidth=2)
        ax.set_xlabel("PPO steps (thousands)")
        ax.set_title(label)
    if max_entropy:
        reference(axes[0], max_entropy, f"uniform policy {max_entropy:.2f}")
    fig.tight_layout()
    return save(fig, path)


def consistent(folds: pd.DataFrame, gate_pf: float = 1.0) -> pd.Series:
    return (folds["test_return_pct"] > 0) & (folds["test_profit_factor"] > gate_pf)


def plot_fold_table(plt, folds: pd.DataFrame, path: Path, gate_pf: float = 1.0, gate_worst_pf: float = 0.9) -> Path:
    """
    Which folds pass? Each test window's Sharpe for the agent and every baseline: 
    the agent's profit factor against the gate's lines, each fold marked consistent or not
    """
    fig, (top, bottom) = plt.subplots(2, 1, figsize=(13, 7.5), sharex=True)
    fig.suptitle("The test windows: the agent against the baselines, and the gate", fontsize=12, fontweight="bold", color=TEXT)
    x = folds["fold"].to_numpy()
    series = [("agent", "test_sharpe"), ("always_long", "always_long_sharpe"), ("trend_hold", "trend_hold_sharpe"), ("random", "random_sharpe")]
    series = [(n, c) for n, c in series if c in folds]
    width = 0.8 / len(series)
    for i, (name, col) in enumerate(series):
        top.bar(x - 0.4 + width * (i + 0.5), folds[col], width=width, color=BASELINE_COLORS[name], edgecolor=PANEL, linewidth=1, label=name.replace("_", " "))
    top.axhline(0, color=TEXT, linewidth=0.8)
    top.set_ylabel("annualized Sharpe of the test window")
    top.set_title("Sharpe per test window (the returns include the trades still open at the window's end, marked at the close)")
    top.legend(fontsize=8, ncol=len(series))
    ok = consistent(folds, gate_pf)
    bottom.bar(x, folds["test_profit_factor"], width=0.6, color=AGENT, edgecolor=PANEL)
    reference(bottom, gate_pf, f"profit factor {gate_pf:g}: a consistent fold needs more, and a positive return", left=True)
    reference(bottom, gate_worst_pf, f"the gate's worst fold: {gate_worst_pf:g}", left=True)
    for xi, pf, good in zip(x, folds["test_profit_factor"], ok):
        bottom.annotate("pass" if good else "fail", xy=(xi, pf), xytext=(0, 4), textcoords="offset points", ha="center", fontsize=8, color=UP if good else DOWN, fontweight="bold")
    bottom.set_ylabel("the agent's profit factor (closed trades)")
    bottom.set_xlabel("fold")
    bottom.set_xticks(x)
    bottom.set_title(f"profit factor per test window: {int(ok.sum())} of {len(folds)} folds consistent")
    fig.tight_layout()
    return save(fig, path)


def plot_oos_equity(plt, agent: pd.DataFrame, baselines: pd.DataFrame | None, path: Path) -> Path:
    """
    Out of sample, does the agent beat the simple rules? Every test window joined, per market: 
    the agent's curve per market (thin), the median over the markets of the agent and of each baseline
    """
    fig, ax = plt.subplots(figsize=(13, 4.8))
    days = agent["test_day"].to_numpy()
    curves = (agent.drop(columns="test_day") - 1.0) * 1e4
    for col in curves.columns:
        ax.plot(days, curves[col], color=AGENT, linewidth=0.6, alpha=0.18)
    ax.plot(days, curves.median(axis=1), color=AGENT, linewidth=2.2, label="agent (median over the markets; thin: each market)")
    if baselines is not None and not baselines.empty:
        for name, g in baselines.groupby("baseline"):
            med = g.pivot_table(index="test_day", columns="seed", values="equity").median(axis=1)
            ax.plot(med.index, (med.values - 1.0) * 1e4, color=BASELINE_COLORS.get(name, MUTED), linewidth=1.8, linestyle=(0, (4, 2)) if name == "flat" else "-", label=f"{name.replace('_', ' ')} (median)")
    ax.axhline(0, color=TEXT, linewidth=0.8)
    ax.set_xlabel("out-of-sample day (the test windows of every fold, one after the other)")
    ax.set_ylabel("bp of the equity")
    ax.set_title("Out of sample, every test window joined: the agent against the baselines (open trades marked at the close)")
    ax.legend(fontsize=8, loc="upper left")
    fig.tight_layout()
    return save(fig, path)


def plot_actions(plt, actions: pd.DataFrame, trades: pd.DataFrame, path: Path) -> Path:
    """
    What does the agent do? Its decisions when flat and when holding, the stop and target of the trades it opens, how those trades end, and their result in R, on every test window
    """
    fig, axes = plt.subplots(2, 3, figsize=(14, 7.5))
    fig.suptitle("What the agent does on the test windows: its decisions, its brackets, how its trades end", fontsize=12, fontweight="bold", color=TEXT)
    names = {0: "flat", 1: "long", -1: "short"}
    for ax, held, title in ((axes[0, 0], 0, "when flat: what it chooses"), (axes[0, 1], 1, "when holding: keep, close or flip")):
        a = actions[actions["held"] != 0] if held else actions[actions["held"] == 0]
        if held:
            kind = np.where(a["direction"] == a["held"], "keep", np.where(a["direction"] == 0, "close", "flip"))
            counts = a.assign(kind=kind).groupby("kind")["count"].sum().reindex(["keep", "close", "flip"]).fillna(0)
        else:
            counts = a.groupby("direction")["count"].sum().rename(index=names).reindex(["flat", "long", "short"]).fillna(0)
        total = max(counts.sum(), 1)
        bars = ax.bar(counts.index, counts.values / total * 100, color=[NEUTRAL, AGENT, ALWAYS_LONG], edgecolor=PANEL)
        for bar, v in zip(bars, counts.values / total * 100):
            ax.annotate(f"{v:.0f}%", xy=(bar.get_x() + bar.get_width() / 2, v), xytext=(0, 3), textcoords="offset points", ha="center", fontsize=8)
        ax.set_ylabel("% of its decisions")
        ax.set_title(f"{title} ({int(total):,} decisions)")
    opened = actions[(actions["held"] == 0) & (actions["direction"] != 0)]
    ax = axes[0, 2]
    grid = opened.pivot_table(index="sl_atr", columns="tp_r", values="count", aggfunc="sum", fill_value=0)
    if not grid.empty:
        im = ax.imshow(grid.to_numpy(), cmap="Blues", aspect="auto")
        ax.set_xticks(range(len(grid.columns)))
        ax.set_xticklabels([f"{v:g} R" for v in grid.columns])
        ax.set_yticks(range(len(grid.index)))
        ax.set_yticklabels([f"{v:g} ATR" for v in grid.index])
        for (i, j), v in np.ndenumerate(grid.to_numpy()):
            ax.text(j, i, f"{int(v)}", ha="center", va="center", fontsize=8, color=BG if v < grid.to_numpy().max() / 2 else TEXT) # dark on the light cells
        ax.grid(False)
    ax.set_xlabel("take-profit (multiples of the stop distance)")
    ax.set_ylabel("stop-loss")
    ax.set_title("the brackets of the trades it opens")
    ax = axes[1, 0]
    if not trades.empty:
        exits = trades["reason"].value_counts()
        ax.bar(exits.index, exits.values, color=[EXIT_COLORS.get(r, MUTED) for r in exits.index], edgecolor=PANEL)
        ax.set_title(f"how its {len(trades):,} trades end")
        ax.set_ylabel("trades")
        ax = axes[1, 1]
        r = trades["r"].clip(-3, 4)
        ax.hist(r, bins=np.linspace(-3, 4, 36), color=AGENT, edgecolor=PANEL)
        reference(ax, float(trades["r"].mean()), f"mean {trades['r'].mean():+.2f} R", horizontal=False)
        ax.set_xlabel("result per trade (R: multiples of the risk taken; clipped to -3..4)")
        ax.set_ylabel("trades")
        ax.set_title("result per trade")
        ax = axes[1, 2]
        by = trades.groupby("reason")["r"].mean().reindex(exits.index)
        ax.bar(by.index, by.values, color=[EXIT_COLORS.get(r, MUTED) for r in by.index], edgecolor=PANEL)
        ax.axhline(0, color=TEXT, linewidth=0.8)
        ax.set_ylabel("mean R")
        ax.set_title("mean result by exit")
    fig.tight_layout()
    return save(fig, path)


def plot_sealed(plt, holdout: dict, path: Path) -> Path:
    """
    Evaluated once, on markets nothing was chosen on: the agent, the ensemble and the baselines side by side
    """
    names = [n for n in ("agent", "ensemble", "always_long", "trend_hold", "random", "flat") if isinstance(holdout.get(n), dict)]
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.2))
    fig.suptitle("The sealed markets, evaluated once: the agent, the ensemble and the baselines", fontsize=12, fontweight="bold", color=TEXT)
    for ax, key, label, scale in ((axes[0], "return_pct", "return (bp of the equity)", 100), (axes[1], "sharpe", "annualized Sharpe", 1),
                                  (axes[2], "profit_factor", "profit factor (closed trades)", 1)):
        values = [holdout[n][key] * scale for n in names]
        bars = ax.bar([n.replace("_", "\n") for n in names], values, color=[BASELINE_COLORS.get(n, MUTED) for n in names], edgecolor=PANEL)
        for bar, v in zip(bars, values):
            ax.annotate(f"{v:+.2f}" if key != "profit_factor" else f"{v:.2f}", xy=(bar.get_x() + bar.get_width() / 2, v), xytext=(0, 3 if v >= 0 else -11), textcoords="offset points", ha="center", fontsize=8)
        if key == "profit_factor":
            reference(ax, 1.0, "1")
        else:
            ax.axhline(0, color=TEXT, linewidth=0.8)
        ax.set_title(label)
    fig.tight_layout()
    return save(fig, path)


def training_report(name: str = "rl", reports: Path | None = None, log=print) -> Path | None:
    """
    Every training chart from reports/<name>_* into reports/<name>/plots/, with its index page
    """
    plt = plotting()
    if plt is None:
        return None
    reports = Path(reports or ROOT / "reports")
    out = reports / name / "plots"
    evals = pd.read_csv(reports / f"{name}_evals.csv")
    folds = pd.read_csv(reports / f"{name}_folds.csv")
    summary = json.loads((reports / f"{name}_summary.json").read_text())
    oos = pd.read_csv(reports / f"{name}_oos_equity.csv")
    read = lambda f: pd.read_csv(reports / f) if (reports / f).exists() else None
    baselines, actions, trades = read(f"{name}_oos_baselines.csv"), read(f"{name}_test_actions.csv"), read(f"{name}_test_trades.csv")
    meta = json.loads((ROOT / "artifacts" / name / "metadata.json").read_text()) if (ROOT / "artifacts" / name / "metadata.json").exists() else {}
    heads = [3, len(meta.get("sl_atr", [1, 1.5, 2])), len(meta.get("tp_r", [1, 1.5, 2, 3]))]
    plot_learning_curves(plt, evals, folds, out / "learning_curves.png")
    plot_ppo_diagnostics(plt, evals, out / "ppo_diagnostics.png", float(np.log(heads).sum()))
    plot_fold_table(plt, folds, out / "folds.png")
    plot_oos_equity(plt, oos, baselines, out / "oos_equity.png")
    if actions is not None and trades is not None:
        plot_actions(plt, actions, trades, out / "actions.png")
    if summary.get("holdout"):
        plot_sealed(plt, summary["holdout"], out / "sealed.png")
    gate = "".join(f"<br><code>{html.escape(line)}</code>" for line in summary.get("gate", []))
    provenance = summary.get("provenance", {})
    missing = "" if actions is not None else " (the actions chart needs a training run that saved its test windows' actions)"
    page = index_page(out, f"Training of {name}", f"Trained on {html.escape(str(provenance.get('source', '?')))}: development markets "
                      f"{html.escape(str(provenance.get('seeds', [])))}, sealed {html.escape(str(provenance.get('sealed_seeds', [])))}. "
                      f"The gate: <b>{'PASSED' if summary.get('gate_passed') else 'FAILED'}</b>{gate}", [
        ("Learning curves", ["learning_curves.png"], "Did each fold find a checkpoint good on both legs? Only a checkpoint profitable on both, trading at least the minimum, is eligible; the kept one has the best weaker leg."),
        ("Is PPO converging?", ["ppo_diagnostics.png"], "Entropy falling from the uniform policy's, a small approximate KL per update, a value function that explains the returns."),
        ("The test windows and the gate", ["folds.png"], "Each fold's test window, never used for any choice."),
        ("Out of sample", ["oos_equity.png"], "The test windows of every fold joined, per development market."),
        ("What the agent does", ["actions.png"], "Its decisions, brackets and exits on the test windows" + missing + "."),
        ("The sealed markets", ["sealed.png"], "Evaluated once, at the end."),
    ])
    log(f"training charts: {page}")
    return page


# =============================================================================================== live
def day_of(elapsed_ms: np.ndarray, daily: pd.DataFrame) -> np.ndarray:
    """
    Elapsed time of the run -> trading day, as a continuous number (day 3.5: halfway through day 3)
    """
    days = daily.drop_duplicates("day").sort_values("day")
    opens, closes, number = days["open_ms"].to_numpy(), days["close_ms"].to_numpy(), days["day"].to_numpy()
    k = np.clip(np.searchsorted(opens, elapsed_ms, side="right") - 1, 0, len(opens) - 1)
    frac = np.clip((elapsed_ms - opens[k]) / np.maximum(closes[k] - opens[k], 1.0), 0.0, 1.0)
    return number[k] + frac


def plot_rl_equity(plt, samples: pd.DataFrame, prices: pd.DataFrame, final: pd.DataFrame, daily: pd.DataFrame, rl_names: set, path: Path) -> Path:
    """
    How did each RL bot's account move, day after day? Above, the market (the symbols' mean close, rebased); below, each RL bot's excess over holding its starting account
    """
    fig, (top, bottom) = plt.subplots(2, 1, figsize=(13, 7.5), sharex=True, gridspec_kw={"height_ratios": [1, 1.8]})
    fig.suptitle("The RL bots over the trading days: the market above, each bot's excess over holding its start below", fontsize=12, fontweight="bold", color=TEXT)
    closes = daily.pivot_table(index="day", columns="symbol", values="close")
    rebased = closes / closes.iloc[0]
    top.plot(rebased.index, (rebased.mean(axis=1) - 1) * 100, color=TEXT, linewidth=1.8, label="the symbols' mean close")
    for sym in rebased.columns:
        top.plot(rebased.index, (rebased[sym] - 1) * 100, color=MUTED, linewidth=0.7, alpha=0.6)
    top.axhline(0, color=MUTED, linewidth=0.8)
    top.set_ylabel("% since day 0")
    top.set_title("the market: each symbol's close (grey) and their mean")
    labels = final.set_index("client")["label"]
    start = final.set_index("client")
    p0 = prices.sort_values("elapsed_ms").groupby("symbol")["last_price"].first()
    px = prices.pivot_table(index="elapsed_ms", columns="symbol", values="last_price").sort_index().ffill()
    colors = {}
    palette = [AGENT, ALWAYS_LONG, TREND_HOLD, RANDOM, "#9085e9", "#e66767", UP]
    for client, g in samples[samples["client"].map(labels).isin(rl_names)].groupby("client"):
        name = labels[client]
        colors.setdefault(name, palette[len(colors) % len(palette)])
        g = g.sort_values("elapsed_ms")
        stock = start.loc[client, "initial_net_worth"] - start.loc[client, "initial_cash"]
        prices_at = px.reindex(g["elapsed_ms"], method="ffill")
        held = start.loc[client, "initial_cash"] + (stock / len(p0)) * (prices_at / p0).sum(axis=1).to_numpy()
        excess = (g["net_worth"].to_numpy() - held) / start.loc[client, "initial_net_worth"] * 100
        bottom.plot(day_of(g["elapsed_ms"].to_numpy(), daily), excess, color=colors[name], linewidth=1.2, alpha=0.9, label=name if name not in [l.get_label() for l in bottom.get_lines()] else None)
    bottom.axhline(0, color=TEXT, linewidth=0.8)
    bottom.set_ylabel("excess over holding (% of the account)")
    bottom.set_xlabel("trading day")
    bottom.set_title("each RL bot (two per agent): what it added over holding its starting account")
    bottom.legend(fontsize=8, ncol=4)
    fig.tight_layout()
    return save(fig, path)


def plot_rl_trades_on_candles(plt, daily: pd.DataFrame, trades: pd.DataFrame, path: Path, symbol: str | None = None) -> Path:
    """
    Where did the RL bots trade? One symbol's daily candles with every RL trade: 
    its entry, its exit marked by reason, its stop and target while it was open
    """
    if symbol is None:
        symbol = trades["symbol"].value_counts().index[0] if not trades.empty else daily["symbol"].iloc[0]
    d = daily[daily["symbol"] == symbol].sort_values("day")
    t = trades[trades["symbol"] == symbol]
    fig, ax = plt.subplots(figsize=(14, 6))
    for k in d.itertuples():
        color = UP if k.close >= k.open else DOWN
        ax.vlines(k.day, k.low, k.high, color=color, linewidth=0.8, alpha=0.55)
        ax.add_patch(plt.Rectangle((k.day - 0.3, min(k.open, k.close)), 0.6, max(abs(k.close - k.open), 1e-9), facecolor=color, edgecolor=color, alpha=0.4))
    for tr in t.itertuples():
        x0, x1 = tr.entry_day - 0.15, tr.exit_day + 0.15
        ax.hlines(tr.stop, x0, x1, color=DOWN, linewidth=0.8, linestyle=(0, (2, 2)), alpha=0.8)
        ax.hlines(tr.target, x0, x1, color=UP, linewidth=0.8, linestyle=(0, (2, 2)), alpha=0.8)
        ax.plot([tr.entry_day, tr.exit_day], [tr.entry, tr.exit], color=TEXT, linewidth=0.6, alpha=0.6)
        ax.scatter([tr.entry_day], [tr.entry], marker="^" if tr.direction > 0 else "v", s=60, color=AGENT, edgecolor=BG, zorder=4)
        ax.scatter([tr.exit_day], [tr.exit], marker="o", s=42, color=EXIT_COLORS.get(tr.reason, MUTED), edgecolor=BG, zorder=4)
    from matplotlib.lines import Line2D
    handles = [Line2D([], [], marker="^", linestyle="", color=AGENT, label="entry, long (v: short)")]
    handles += [Line2D([], [], marker="o", linestyle="", color=c, label=f"exit: {r}") for r, c in EXIT_COLORS.items() if r in set(t["reason"])]
    handles += [Line2D([], [], color=DOWN, linestyle=(0, (2, 2)), label="stop-loss"), Line2D([], [], color=UP, linestyle=(0, (2, 2)), label="take-profit")]
    ax.legend(handles=handles, fontsize=8, ncol=3, loc="upper left")
    ax.set_xlabel("trading day")
    ax.set_ylabel("price")
    ax.set_title(f"{symbol}: the daily candles and every RL trade on it ({len(t)} trades, every RL bot)")
    fig.tight_layout()
    return save(fig, path)


def execution_costs(trades: pd.DataFrame, daily: pd.DataFrame) -> tuple[pd.Series, pd.Series, pd.Series]:
    """
    Per trade, in bp against the trade: the entry against the day's open (a market-on-open order fills in the opening auction), 
    a stop-loss's fill past its stop, a take-profit's fill against its target
    """
    opens = daily.set_index(["day", "symbol"])["open"]
    entry_open = opens.reindex(pd.MultiIndex.from_arrays([trades["entry_day"], trades["symbol"]])).to_numpy()
    entry_bp = pd.Series(1e4 * (trades["entry"].to_numpy() / entry_open - 1) * trades["direction"].to_numpy()).dropna()
    sl = trades[trades["reason"] == "SL"]
    stop_bp = 1e4 * (sl["stop"] - sl["exit"]) * sl["direction"] / sl["stop"]
    tp = trades[trades["reason"] == "TP"]
    target_bp = 1e4 * (tp["target"] - tp["exit"]) * tp["direction"] / tp["target"]
    return entry_bp, stop_bp, target_bp


def plot_execution(plt, costs: tuple[pd.Series, pd.Series, pd.Series], path: Path, assumed_stop_bp: float = 100.0, assumed_entry_bp: float | None = None) -> Path:
    """
    Does live match training? The real fills against what the environment assumes: 
    entries against the open, stop-losses past their stop, take-profits against their target (bp against the trade: positive = worse)
    costs: execution_costs() of one market, or several concatenated
    """
    entry_bp, stop_bp, target_bp = costs
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.4))
    fig.suptitle("Execution: the live fills against the training environment's assumptions (bp against the trade, positive = worse)", fontsize=12, fontweight="bold", color=TEXT)
    for ax, values, label, assumed, assumed_text in ((axes[0], entry_bp, "entry against the day's open", assumed_entry_bp, "assumed"),
                                                     (axes[1], stop_bp, "stop-loss fill past the stop", assumed_stop_bp, "assumed"),
                                                     (axes[2], target_bp, "take-profit fill against the target", 0.0, "assumed")):
        if len(values):
            lo, hi = np.percentile(values, [1, 99])
            ax.hist(values.clip(lo, hi), bins=40, color=AGENT, edgecolor=PANEL)
            reference(ax, float(values.median()), f"live median {values.median():+.0f}", horizontal=False)
            if assumed is not None:
                ax.axvline(assumed, color=ALWAYS_LONG, linewidth=1.6)
                ax.annotate(f"{assumed_text} {assumed:+.0f}", xy=(assumed, 0.9), xycoords=("data", "axes fraction"), xytext=(3, 0), textcoords="offset points", fontsize=8, color=ALWAYS_LONG)
            ax.set_title(f"{label}: {len(values)} fills, mean {values.mean():+.0f} bp")
        else:
            ax.set_title(f"{label}: no fill")
        ax.set_xlabel("bp against the trade")
        ax.set_ylabel("fills")
    fig.tight_layout()
    return save(fig, path)


def plot_activity(plt, live: pd.DataFrame, days: dict, path: Path) -> Path:
    """How much does each RL agent trade? Its entries per trading day per bot, one dot per market, the bar their mean."""
    rl = live.dropna(subset=["entries"]).copy()
    rl["per_day"] = rl["entries"] / (rl["bots"] * rl["seed"].map(days))
    order = rl.groupby("strategy")["per_day"].mean().sort_values()
    fig, ax = plt.subplots(figsize=(11, 0.5 * len(order) + 1.6))
    ax.barh(range(len(order)), order.values, color=AGENT, edgecolor=PANEL, height=0.6)
    for i, name in enumerate(order.index):
        v = rl.loc[rl["strategy"] == name, "per_day"]
        ax.scatter(v, np.full(len(v), i), s=20, color=TEXT, edgecolor=PANEL, zorder=3)
    ax.set_yticks(range(len(order)))
    ax.set_yticklabels(order.index)
    ax.set_xlabel("trades opened per trading day, per bot (all 5 symbols)")
    ax.set_title("How much each RL agent trades: entries per day per bot, one dot per market")
    fig.tight_layout()
    return save(fig, path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stage", choices=["training"])
    parser.add_argument("--name", default="rl")
    parser.add_argument("--reports", type=Path, default=ROOT / "reports")
    args = parser.parse_args()
    training_report(args.name, args.reports)


if __name__ == "__main__":
    main()
