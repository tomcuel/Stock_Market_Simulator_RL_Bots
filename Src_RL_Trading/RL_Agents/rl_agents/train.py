"""
Walk-forward training of the RL agent (our own PPO, ppo.py) on recorded trading days, with the XAUUSD reference project's validation protocol, on the simulated exchange

Usage (from RL_Agents/):
    python -m rl_agents.train --data sim_days/real5_d600 --jobs 8
    python -m rl_agents.train --data sim_days/real5_d600 --timesteps 300000 --seeds 2 --ent-coef 0.01 --jobs 8
    python -m rl_agents.train --data sim_days/real5_d600 --train-days 90 --val-days 20 --test-days 20 --step-days 20
                                                   # run 1 to 3's fold sizes: 18 shorter folds

The protocol:
1. Markets: Every recorded run (seed) is a market, the last --holdout seeds are sealed: no training, no checkpoint, no tuning ever sees them: 
   they are evaluated once, at the end (the reference project's sealed test split).
2. Sliding walk-forward on the day index, the same calendar for every development market: each fold trains on
   --train-days days (180), picks its checkpoint on the next --val-days (30), is judged on the following --test-days (30), 
   then the window slides by --step-days (30), with an embargo of --embargo days at each boundary: on 600-day markets, 12 folds and 360 out-of-sample days per market. 
   What this simulates: retrain every step_days, trade the next test_days.
   Every fold's test window joins one continuous out-of-sample curve per market.
3. Checkpoint choice (the consistency rule): every --eval-every steps the greedy policy is run on a train-eval slice (the last val_days of the training window) 
   and on the validation window, each leg scores return% - dd_penalty x maxDD%, the checkpoint's score is the WEAKER leg. 
   Only a checkpoint profitable on both legs that trades at least --min-trades times can become the best (an idle policy has no drawdown, it must not win by doing nothing).
4. Gate: the fold models ship (gate_passed=true in their metadata) only if enough folds are consistent on their test
   window (return > 0 and profit factor > 1), the worst fold's profit factor is >= 0.9 and the mean Sharpe is > 0. 
   The policy server refuses an agent that failed it unless asked (--allow-ungated).
5. Ensemble: the last --ensemble fold models, each trading its own book, equal capital (averaging their actions instead makes incoherent trades). 
   The simulator runs it as one RL bot per member.
6. Baselines on the same windows: flat, always long (a fixed bracket), a trend-hold rule tuned on train/validation only, the random policy, and buy-and-hold.

Writes artifacts/<name> (the last fold's model: production), artifacts/<name>_f<k> (every fold model, the ensemble
members), reports/<name>_folds.csv, _evals.csv, _oos_equity.csv, _summary.json, and for the charts _oos_baselines.csv
(the baselines' out-of-sample curves), _test_actions.csv and _test_trades.csv (the agent on the test windows), then the
training charts and their page, reports/<name>/plots/index.html (rl_agents.plots).

--jobs N trains the folds (and their PPO seeds) in N processes at once: every training is seeded, so the result is the same as with --jobs 1. 
The logs show the agent's returns in bp: it risks 0.2% of the equity per trade.
"""
from __future__ import annotations

import argparse
import json
import multiprocessing
import os
import pickle
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

from .env import DEFAULTS as ENV_DEFAULTS
from .env import HEADS, SL_ATR, TP_R, RLEnv, obs_dim, rollout
from .daily import FEATURES, WARMUP, Stream, feature_audit, load_market, sliding_folds
from .paths import display_path
from .ppo import PPOAgent, PPOConfig

ROOT = Path(__file__).resolve().parent.parent
PERIODS = 252


# ---------------------------------------------------------------- metrics
def curve_metrics(curve: np.ndarray, trades: list[dict]) -> dict:
    curve = np.concatenate([[1.0], np.asarray(curve, dtype=float)])
    r = np.diff(curve) / curve[:-1]
    peak = np.maximum.accumulate(curve)
    gains = sum(t["pnl"] for t in trades if t["pnl"] > 0)
    losses = -sum(t["pnl"] for t in trades if t["pnl"] < 0)
    return {"return_pct": 100 * (curve[-1] - 1), "sharpe": float(r.mean() / r.std() * np.sqrt(PERIODS)) if r.std() > 0 else 0.0,
            "max_dd_pct": float(100 * np.max((peak - curve) / peak)), "trades": len(trades),
            "profit_factor": float(gains / losses) if losses > 0 else (float("inf") if gains > 0 else 0.0),
            "win_rate": float(np.mean([t["pnl"] > 0 for t in trades])) if trades else 0.0,
            "mean_r": float(np.mean([t["r"] for t in trades])) if trades else 0.0}


def book(policy, streams: list[Stream], window, env_config) -> tuple[dict, dict, list]:
    """Runs a policy on every stream of the window; each market (seed) is a portfolio of its symbols, equal capital.
    Returns (pooled metrics, {seed: portfolio curve}, every trade)."""
    by_seed, trades = {}, []
    for s in streams:
        curve, t = rollout(policy, s, window, env_config)
        by_seed.setdefault(s.seed, []).append(curve)
        trades.extend(t)
    portfolios = {}
    for seed, curves in by_seed.items():
        n = min(len(c) for c in curves)
        portfolios[seed] = np.mean([c[:n] for c in curves], axis=0)
    pooled = np.mean([p[:min(len(q) for q in portfolios.values())] for p in portfolios.values()], axis=0) if portfolios else np.array([1.0])
    m = curve_metrics(pooled, trades)
    m["per_market_return_pct"] = {int(k): float(100 * (v[-1] - 1)) for k, v in portfolios.items()}
    return m, portfolios, trades


def recording(policy, decisions: list, fold: int):
    """The same policy, every decision written down: (fold, the open trade's direction, the chosen direction, stop, target)."""
    def wrapped(obs, env):
        action = policy(obs, env)
        decisions.append((fold, env.trade.direction, {0: 0, 1: 1, 2: -1}[int(action[0])], SL_ATR[int(action[1])], TP_R[int(action[2])]))
        return action
    return wrapped


def stitch(oos: dict) -> dict:
    """One continuous out-of-sample curve per market: each fold's test window compounds on the previous one."""
    out = {}
    for seed, curves in oos.items():
        level, series = 1.0, []
        for c in curves:
            rel = np.concatenate([[1.0], c])
            series.extend(level * rel[1:])
            level *= rel[-1]
        out[seed] = series
    return out


def agent_policy(agent: PPOAgent):
    return lambda obs, env: agent.act(obs, deterministic=True)[0]


# ---------------------------------------------------------------- baselines
def flat_policy(obs, env):
    return [0, 0, 0]


def long_policy(obs, env):
    return [1, 1, 1] # long, stop 1.5 ATR, target 1.5 R


def random_policy_factory(seed: int):
    rng = np.random.default_rng(seed)
    return lambda obs, env: [int(rng.integers(n)) for n in HEADS]


def trend_hold_factory(threshold: float, sl_k: int, tp_k: int):
    """Long above the 20-day trend, short below it, holding until the stop, the target or a flip (the reference
    project's trend-hold baseline, on the daily features)."""
    i_gap, i_trend = FEATURES.index("close_ema5_atr"), FEATURES.index("ema5_ema20_atr")

    def policy(obs, env):
        trend, gap = obs[i_trend], obs[i_gap]
        if trend > 0 and gap > threshold:
            return [1, sl_k, tp_k]
        if trend < 0 and gap < -threshold:
            return [2, sl_k, tp_k]
        held = env.trade.direction
        return [1 if held == 1 else 2 if held == -1 else 0, sl_k, tp_k]
    return policy


def tune_trend_hold(train, val, streams, env_config) -> tuple[dict, float]:
    """Grid on train + validation only (the test window stays sealed): the weaker leg's return wins, like the agent."""
    best, best_score = None, -np.inf
    for threshold in (0.0, 0.3, 0.6, 0.9):
        for sl_k in range(len(SL_ATR)):
            for tp_k in (1, 2, 3):
                p = trend_hold_factory(threshold, sl_k, tp_k)
                score = min(book(p, streams, train, env_config)[0]["return_pct"], book(p, streams, val, env_config)[0]["return_pct"])
                if score > best_score:
                    best, best_score = {"threshold": threshold, "sl_k": sl_k, "tp_k": tp_k}, score
    return best, best_score


def buy_and_hold(streams: list[Stream], window) -> float:
    lo, hi = window
    rets = [s.close[min(hi, s.n_days) - 1] / s.close[lo - 1] - 1 for s in streams]
    return 100 * float(np.mean(rets))


# ---------------------------------------------------------------- one fold
def train_fold(fold, dev, args, env_config, seed: int, log) -> dict:
    cfg = PPOConfig(hidden=tuple(args.hidden), learning_rate=args.lr, n_steps=args.n_steps, batch_size=args.batch_size,
                    n_epochs=args.n_epochs, gamma=args.gamma, clip_range=args.clip, ent_coef=args.ent_coef,
                    target_kl=args.target_kl, weight_decay=args.weight_decay, seed=seed)
    agent = PPOAgent(obs_dim(), HEADS, cfg)
    rng = np.random.default_rng(seed)
    train_eval = (max(fold.train[0], fold.train[1] - (fold.val[1] - fold.val[0])), fold.train[1])
    factory = lambda: RLEnv(dev, fold.train, env_config, rng=rng, random_start=True)
    state, evals = {}, []
    best, best_score = None, -np.inf
    started = time.time()
    while agent.timesteps < args.timesteps:
        progress = 1.0 - agent.timesteps / args.timesteps
        rollout_data = agent.collect(factory, cfg.n_steps, state)
        stats = agent.update(rollout_data, progress)
        if agent.timesteps % args.eval_every < cfg.n_steps or agent.timesteps >= args.timesteps:
            policy = agent_policy(agent)
            tr = book(policy, dev, train_eval, env_config)[0]
            va = book(policy, dev, fold.val, env_config)[0]
            q_train = tr["return_pct"] - args.dd_penalty * tr["max_dd_pct"]
            q_val = va["return_pct"] - args.dd_penalty * va["max_dd_pct"]
            score = min(q_train, q_val)
            eligible = tr["return_pct"] > 0 and va["return_pct"] > 0 and tr["trades"] >= args.min_trades and va["trades"] >= args.min_trades
            marker = ""
            if eligible and score > best_score:
                best, best_score, marker = agent.snapshot(), score, "  <- best"
            evals.append({"fold": fold.k, "seed": seed, "timesteps": agent.timesteps, "train_eval_ret": tr["return_pct"], "val_ret": va["return_pct"],
                          "train_dd": tr["max_dd_pct"], "val_dd": va["max_dd_pct"], "train_trades": tr["trades"], "val_trades": va["trades"],
                          "score": score, "eligible": eligible, "entropy": stats["entropy"], "approx_kl": stats["approx_kl"],
                          "explained_variance": stats["explained_variance"]})
            log(f"    fold {fold.k} seed {seed} [{agent.timesteps:>7,}] train {bp(tr['return_pct'])}/dd {100 * tr['max_dd_pct']:4.1f} bp ({tr['trades']:4d} trades)  "
                f"val {bp(va['return_pct'])}/dd {100 * va['max_dd_pct']:4.1f} bp ({va['trades']:4d})  score {score:+6.2f}{'' if eligible else '  (ineligible)'}{marker}"
                f"  entropy {stats['entropy']:.2f}  {time.time() - started:.0f}s")
    chosen, kind = (best, "best eligible checkpoint") if best is not None else (agent, "last checkpoint (none eligible)")
    return {"agent": chosen, "kind": kind, "score": best_score if best is not None else None, "evals": evals}


def bp(return_pct: float) -> str:
    return f"{100 * return_pct:+7.2f} bp"


# ---------------------------------------------------------------- the trainings, in parallel processes (--jobs)
_WORKER: dict = {}


def _init_worker(dev, args, env_config) -> None:
    _WORKER.update(dev=dev, args=args, env_config=env_config)


def _train_job(job) -> dict:
    """One (fold, PPO seed) training; the agent comes back as its state (plain arrays), whichever process ran it."""
    fold, seed = job
    result = train_fold(fold, _WORKER["dev"], _WORKER["args"], _WORKER["env_config"], seed, lambda m: print(m, flush=True))
    result["agent"] = result["agent"].state()
    return result


def train_all(jobs: list, dev, args, env_config) -> list[dict]:
    """Every (fold, seed) training, serially or in args.jobs processes: the same results either way."""
    if getattr(args, "jobs", 1) <= 1:
        _init_worker(dev, args, env_config)
        results = [_train_job(job) for job in jobs]
    else:
        for var in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "VECLIB_MAXIMUM_THREADS", "MKL_NUM_THREADS"):
            os.environ.setdefault(var, "1") # one core per process: the networks are small
        with ProcessPoolExecutor(max_workers=args.jobs, mp_context=multiprocessing.get_context("spawn"),
                                 initializer=_init_worker, initargs=(dev, args, env_config)) as pool:
            results = list(pool.map(_train_job, jobs))
    for r in results:
        r["agent"] = PPOAgent.from_state(r["agent"])
    return results


# ---------------------------------------------------------------- saving
def save_agent(folder: Path, agent: PPOAgent, meta: dict) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    with open(folder / "ppo.pkl", "wb") as f:
        pickle.dump(agent.state(), f)
    (folder / "metadata.json").write_text(json.dumps(meta, indent=2, default=float) + "\n")


def gate(rows: list[dict], args) -> tuple[bool, list[str]]:
    good = sum(1 for r in rows if r["test_return_pct"] > 0 and r["test_profit_factor"] > args.gate_pf)
    need = args.gate_folds if args.gate_folds > 0 else int(np.ceil(0.75 * len(rows)))
    worst = min(r["test_profit_factor"] for r in rows)
    mean_sharpe = float(np.mean([r["test_sharpe"] for r in rows]))
    checks = [(good >= need, f"folds with test return > 0 and PF > {args.gate_pf:g}: {good}/{len(rows)} (need >= {need})"),
              (worst >= args.gate_worst_pf, f"worst fold PF {worst:.2f} (need >= {args.gate_worst_pf:g})"),
              (mean_sharpe > 0, f"mean test Sharpe {mean_sharpe:+.2f} (need > 0)")]
    return all(ok for ok, _ in checks), [("[PASS] " if ok else "[FAIL] ") + text for ok, text in checks]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", required=True, help="a folder of trading-day runs (scripts/simulate_market_days.sh)")
    parser.add_argument("--name", default="rl")
    parser.add_argument("--holdout", type=int, default=8, help="sealed markets: the last N seeds, evaluated once at the end")
    parser.add_argument("--train-days", type=int, default=180)
    parser.add_argument("--val-days", type=int, default=30)
    parser.add_argument("--test-days", type=int, default=30)
    parser.add_argument("--step-days", type=int, default=30)
    parser.add_argument("--embargo", type=int, default=2)
    parser.add_argument("--timesteps", type=int, default=200_000, help="PPO steps per fold (and per seed)")
    parser.add_argument("--seeds", type=int, default=1, help="PPO seeds per fold: the best VALIDATION score is kept (never the test)")
    parser.add_argument("--eval-every", type=int, default=20_480)
    parser.add_argument("--dd-penalty", type=float, default=0.5, help="score units per 1%% of max drawdown")
    parser.add_argument("--min-trades", type=int, default=10)
    parser.add_argument("--ensemble", type=int, default=3)
    parser.add_argument("--jobs", type=int, default=1, help="trainings run at once, one process each (the same results as 1)")
    parser.add_argument("--output-root", type=Path, default=ROOT, help="where artifacts/ and reports/ go (default RL_Agents/)")
    # PPO (the reference project's generalisation-first preset, a higher learning rate for the smaller daily data set)
    parser.add_argument("--hidden", type=int, nargs="+", default=[128, 64])
    parser.add_argument("--lr", type=float, default=3e-4)
    parser.add_argument("--n-steps", type=int, default=2048)
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--n-epochs", type=int, default=5)
    parser.add_argument("--gamma", type=float, default=0.95, help="a trade lasts days, not hours: a shorter horizon than 0.99")
    parser.add_argument("--clip", type=float, default=0.1)
    parser.add_argument("--ent-coef", type=float, default=0.03)
    parser.add_argument("--target-kl", type=float, default=0.025)
    parser.add_argument("--weight-decay", type=float, default=1e-5)
    # the environment
    parser.add_argument("--risk-fraction", type=float, default=ENV_DEFAULTS["risk_fraction"])
    parser.add_argument("--slippage-bps", type=float, default=ENV_DEFAULTS["slippage_bps"])
    parser.add_argument("--stop-slippage-bps", type=float, default=ENV_DEFAULTS["stop_slippage_bps"], help="a stop-loss's cost past the stop, measured live")
    parser.add_argument("--episode-days", type=int, default=ENV_DEFAULTS["max_steps"])
    # the gate
    parser.add_argument("--gate-folds", type=int, default=0, help="folds that must pass (default: 75%% of them)")
    parser.add_argument("--gate-pf", type=float, default=1.0)
    parser.add_argument("--gate-worst-pf", type=float, default=0.9)
    args = parser.parse_args()
    log = lambda m: print(m, flush=True)
    output_root = args.output_root.resolve()

    data = Path(args.data).resolve()
    streams = load_market(data)
    seeds = sorted({s.seed for s in streams})
    if len(seeds) <= args.holdout:
        raise SystemExit(f"{len(seeds)} markets in {display_path(data)}: need more than --holdout {args.holdout}")
    sealed = seeds[-args.holdout:] if args.holdout > 0 else []
    dev = [s for s in streams if s.seed not in sealed]
    held = [s for s in streams if s.seed in sealed]
    n_days = min(s.n_days for s in streams)
    folds = sliding_folds(n_days, args.train_days, args.val_days, args.test_days, args.step_days, args.embargo)
    if not folds:
        raise SystemExit(f"{n_days} days per market: too short for train {args.train_days} + val {args.val_days} + test {args.test_days} after {WARMUP} warm-up days")
    env_config = {"risk_fraction": args.risk_fraction, "slippage_bps": args.slippage_bps, "stop_slippage_bps": args.stop_slippage_bps, "max_steps": args.episode_days}
    log(f"{len(seeds)} markets from {display_path(data)} ({n_days} days, {len({s.symbol for s in streams})} symbols): "
        f"development seeds {[x for x in seeds if x not in sealed]}, sealed {sealed}")
    log(f"{len(FEATURES)} market features + 6 position features, every one kept. Near-duplicate pairs (|r| >= 0.96): "
        f"{feature_audit(pd.concat([s.days.iloc[WARMUP:] for s in dev])) or 'none'}")
    for f in folds:
        log(f"  fold {f.k}: train days {f.train}, validation {f.val}, test {f.test}")

    rows, evals, oos, members = [], [], {}, []
    baseline_oos, decisions, all_test_trades = {"always_long": {}, "trend_hold": {}, "flat": {}}, [], [] # for the charts (plots.py)
    jobs = [(fold, seed) for fold in folds for seed in range(1, args.seeds + 1)]
    log(f"\n{len(jobs)} trainings ({len(folds)} folds x {args.seeds} PPO seeds), {args.jobs} at a time")
    trained = train_all(jobs, dev, args, env_config)
    for fold in folds:
        log(f"\n== fold {fold.k}")
        results = [r for (f, _), r in zip(jobs, trained) if f.k == fold.k]
        evals.extend(e for r in results for e in r["evals"])
        scored = [r for r in results if r["score"] is not None]
        chosen = max(scored, key=lambda r: r["score"]) if scored else results[0] # by VALIDATION score only
        agent = chosen["agent"]
        test, portfolios, test_trades = book(recording(agent_policy(agent), decisions, fold.k), dev, fold.test, env_config)
        all_test_trades.extend({"fold": fold.k, **t} for t in test_trades)
        for seed, curve in portfolios.items():
            oos.setdefault(seed, []).append(curve)
        trend_params, _ = tune_trend_hold(fold.train, fold.val, dev, env_config)
        books = {"flat": book(flat_policy, dev, fold.test, env_config), "always_long": book(long_policy, dev, fold.test, env_config),
                 "trend_hold": book(trend_hold_factory(**trend_params), dev, fold.test, env_config),
                 "random": book(random_policy_factory(fold.k), dev, fold.test, env_config)}
        baselines = {name: b[0] for name, b in books.items()}
        for name in baseline_oos:
            for seed, curve in books[name][1].items():
                baseline_oos[name].setdefault(seed, []).append(curve)
        reasons = pd.Series([t["reason"] for t in test_trades]).value_counts().to_dict() if test_trades else {}
        row = {"fold": fold.k, "train": fold.train, "val": fold.val, "test": fold.test, "checkpoint": chosen["kind"], "val_score": chosen["score"],
               **{f"test_{k}": v for k, v in test.items() if k != "per_market_return_pct"}, "test_exits": reasons,
               "buy_and_hold_pct": buy_and_hold(dev, fold.test), "trend_hold_params": trend_params,
               **{f"{name}_return_pct": m["return_pct"] for name, m in baselines.items()},
               **{f"{name}_sharpe": m["sharpe"] for name, m in baselines.items()}}
        rows.append(row)
        log(f"  fold {fold.k} TEST: agent {bp(test['return_pct'])} Sharpe {test['sharpe']:+.2f} PF {test['profit_factor']:.2f} maxDD {100 * test['max_dd_pct']:.1f} bp "
            f"({test['trades']} trades, exits {reasons}) | always long {bp(baselines['always_long']['return_pct'])}  trend-hold {bp(baselines['trend_hold']['return_pct'])}  "
            f"random {bp(baselines['random']['return_pct'])}  buy&hold {row['buy_and_hold_pct']:+.2f}%")
        members.append((fold, agent, row))

    passed, detail = gate(rows, args)
    log("\nconsistency gate (test windows):\n  " + "\n  ".join(detail))

    # sealed markets: the production model (the last fold's), the ensemble of the last folds, the baselines, once
    last = folds[-1]
    sealed_window = (WARMUP + 1, n_days)
    holdout = {}
    if held:
        production = members[-1][1]
        holdout["agent"] = book(agent_policy(production), held, sealed_window, env_config)[0]
        ens = [m[1] for m in members[-args.ensemble:]]
        curves = [book(agent_policy(a), held, sealed_window, env_config) for a in ens]
        n = min(len(c[1][s]) for c in curves for s in c[1])
        blend = np.mean([np.mean([c[1][s][:n] for s in c[1]], axis=0) for c in curves], axis=0)
        holdout["ensemble"] = curve_metrics(blend, [t for c in curves for t in c[2]])
        trend_params, _ = tune_trend_hold(last.train, last.val, dev, env_config)
        for name, p in {"always_long": long_policy, "trend_hold": trend_hold_factory(**trend_params), "flat": flat_policy,
                        "random": random_policy_factory(99)}.items():
            holdout[name] = book(p, held, sealed_window, env_config)[0]
        holdout["buy_and_hold_pct"] = buy_and_hold(held, sealed_window)
        log(f"\nsealed markets {sealed} (days {sealed_window}, evaluated once):")
        for name, m in holdout.items():
            if isinstance(m, dict):
                log(f"  {name:12s} {bp(m['return_pct'])}  Sharpe {m['sharpe']:+5.2f}  PF {m['profit_factor']:5.2f}  maxDD {100 * m['max_dd_pct']:6.1f} bp  {m['trades']} trades")
        log(f"  buy & hold   {holdout['buy_and_hold_pct']:+7.2f}%")

    # artifacts
    provenance = {"kind": "simulation", "source": display_path(data), "seeds": [x for x in seeds if x not in sealed], "sealed_seeds": sealed, "seed": None}
    common = {"agent": "ppo", "features": FEATURES, "warmup": WARMUP, "sl_atr": list(SL_ATR), "tp_r": list(TP_R),
              "env": {**ENV_DEFAULTS, **env_config}, "gate_passed": passed, "gate": detail, "provenance": provenance,
              "ppo": {k: getattr(args, k) for k in ("hidden", "lr", "n_steps", "batch_size", "n_epochs", "gamma", "clip", "ent_coef", "target_kl", "weight_decay", "timesteps")}}
    for fold, agent, row in members:
        save_agent(output_root / "artifacts" / f"{args.name}_f{fold.k}", agent, {**common, "name": f"{args.name}_f{fold.k}", "fold": row})
    # the baselines, served live next to the agent (policy.RuleModel): the last fold's tuned trend-hold, always long
    trend_params, _ = tune_trend_hold(last.train, last.val, dev, env_config)
    for rule, params in (("always_long", {"sl_k": 1, "tp_k": 1}), ("trend_hold", trend_params)):
        folder = output_root / "artifacts" / f"{args.name}_{rule}"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "metadata.json").write_text(json.dumps({**common, "agent": "rule", "name": f"{args.name}_{rule}", "rule": rule, "params": params,
                                                          "gate_passed": True}, indent=2, default=float) + "\n")
    save_agent(output_root / "artifacts" / args.name, members[-1][1], {**common, "name": args.name, "fold": members[-1][2], "holdout": holdout,
                                                                "ensemble_members": [f"{args.name}_f{f.k}" for f, _, _ in members[-args.ensemble:]]})
    reports = output_root / "reports"
    reports.mkdir(exist_ok=True)
    pd.DataFrame(rows).to_csv(reports / f"{args.name}_folds.csv", index=False)
    pd.DataFrame(evals).to_csv(reports / f"{args.name}_evals.csv", index=False)
    out = {f"seed_{seed}": series for seed, series in stitch(oos).items()}
    n = min(len(v) for v in out.values())
    pd.DataFrame({k: v[:n] for k, v in out.items()}).to_csv(reports / f"{args.name}_oos_equity.csv", index_label="test_day")
    summary = {"folds": rows, "gate_passed": passed, "gate": detail, "holdout": holdout, "provenance": provenance}
    (reports / f"{args.name}_summary.json").write_text(json.dumps(summary, indent=2, default=lambda o: float(o) if isinstance(o, (np.floating, np.integer)) else str(o)) + "\n")
    # for the charts: the baselines' out-of-sample curves, the agent's decisions and trades on the test windows
    pd.DataFrame([{"baseline": name, "seed": seed, "test_day": d, "equity": v} for name, by_seed in baseline_oos.items()
                  for seed, series in stitch(by_seed).items() for d, v in enumerate(series[:n])]).to_csv(reports / f"{args.name}_oos_baselines.csv", index=False)
    (pd.DataFrame(decisions, columns=["fold", "held", "direction", "sl_atr", "tp_r"]).groupby(["fold", "held", "direction", "sl_atr", "tp_r"])
       .size().rename("count").reset_index().to_csv(reports / f"{args.name}_test_actions.csv", index=False))
    pd.DataFrame(all_test_trades).to_csv(reports / f"{args.name}_test_trades.csv", index=False)
    log(f"\nsaved {display_path(output_root / 'artifacts' / args.name)} (production, gate {'passed' if passed else 'FAILED'}), "
        f"{len(members)} fold models {args.name}_f1..{members[-1][0].k}, reports in {display_path(reports)}/{args.name}_*")
    try:
        from .plots import training_report
        training_report(args.name, reports, log)
    except Exception as error: # the charts never fail a training run
        log(f"training charts skipped: {type(error).__name__}: {error}")


if __name__ == "__main__":
    main()
