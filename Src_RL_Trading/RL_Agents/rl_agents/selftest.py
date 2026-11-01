"""
Checks the whole package in one command

Usage (from RL_Agents/):  python -m rl_agents.selftest

Covers: every module imports, the daily features (no look-ahead), the market's calibration statistics, the walk-forward
folds (the 600-day calendar), parallel training giving the same agents as serial, the environment's stops and targets, our
PPO (every gradient against finite differences, learning to go long in a rising market, the same action after reload), and
the policy server: its RL protocol and its gate, then the whole exchange of commands over a real socket.
Exit status 1 if anything failed.
"""
from __future__ import annotations

import socket
import sys
import tempfile
import threading
from pathlib import Path

import numpy as np
import pandas as pd

from .protocol import recv_frame, send_frame
from .serve import PolicyService, load_policies, make_server

results: list[tuple[str, str, str]] = []


def check(name, ok, detail=""):
    results.append((name, "PASS" if ok else "FAIL", detail))


def guarded(name):
    """Runs a check function, turning an unexpected exception into a FAIL instead of a crash."""
    def wrap(fn):
        def run(*a, **k):
            try:
                fn(*a, **k)
            except Exception as error:
                check(name, False, repr(error)[:160])
        return run
    return wrap


def rl_artifact(folder: Path, name: str, gate_passed: bool) -> Path:
    """A small untrained PPO RL agent saved as train.py saves one."""
    import json
    import pickle
    from .env import HEADS, obs_dim
    from .ppo import PPOAgent, PPOConfig
    target = folder / name
    target.mkdir(parents=True)
    with open(target / "ppo.pkl", "wb") as f:
        pickle.dump(PPOAgent(obs_dim(), HEADS, PPOConfig(hidden=(8,), seed=1)).state(), f)
    meta = {"agent": "ppo", "warmup": 20, "sl_atr": [1.0, 1.5, 2.0], "tp_r": [1.0, 1.5, 2.0, 3.0], "gate_passed": gate_passed,
            "provenance": {"kind": "simulation", "seeds": [301, 302]}}
    (target / "metadata.json").write_text(json.dumps(meta))
    return target


def synthetic_days(n_days=80, seed=0, drift=0.002):
    """One symbol's trading days (a drifting random walk) and its intraday trades, as daily.load_run builds them."""
    from .daily import Stream, add_features
    rng = np.random.default_rng(seed)
    close = 100 * np.exp(np.cumsum(drift + 0.01 * rng.standard_normal(n_days)))
    open_ = np.concatenate([[100.0], close[:-1]]) * np.exp(0.003 * rng.standard_normal(n_days))
    high, low = np.maximum(open_, close) * 1.006, np.minimum(open_, close) * 0.994
    bars = pd.DataFrame({"day": range(n_days), "open": open_, "high": high, "low": low, "close": close, "volume": 1000 + rng.integers(0, 200, n_days),
                         "trades": 150 + rng.integers(0, 30, n_days), "bid": close * 0.999, "ask": close * 1.001, "bid_depth": 500.0, "ask_depth": 400.0})
    intraday = [np.array([o, l, h, c]) if c >= o else np.array([o, h, l, c]) for o, h, l, c in zip(open_, high, low, close)]
    return bars, Stream("seed_1", 1, "SYN", add_features(bars), intraday)


MODULES = ("calibration", "daily", "env", "paths", "plots", "policy", "ppo", "protocol", "serve", "train")


@guarded("every module of the package imports")
def modules_import():
    import importlib
    names = sorted(p.stem for p in Path(__file__).parent.glob("*.py") if p.stem not in ("__init__", "selftest"))
    for name in names:
        importlib.import_module(f"rl_agents.{name}")
    check("every module of the package imports", tuple(names) == MODULES, f"{len(names)} modules: {' '.join(names)}")


@guarded("trading days: daily features without look-ahead, folds, a near-duplicate audit")
def daily_features():
    from .daily import FEATURES, WARMUP, feature_audit, leakage_check, sliding_folds
    bars, stream = synthetic_days()
    worst = max(leakage_check(bars, cut) for cut in (25, 40, 60))
    folds = sliding_folds(250, 90, 20, 20, 20, embargo=2)
    contiguous = all(f.val[0] == f.train[1] + 2 and f.test[0] == f.val[1] + 2 and f.test[1] <= 250 for f in folds)
    ok = worst == 0.0 and len(folds) == 5 and folds[0].train[0] == WARMUP and contiguous and np.isfinite(stream.x[WARMUP:]).all()
    check("trading days: daily features without look-ahead, folds, a near-duplicate audit", ok,
          f"{len(FEATURES)} features, {len(folds)} folds, audit {feature_audit(stream.days.iloc[WARMUP:]) or 'no duplicates'}")


@guarded("calibration: a market's daily statistics, the 600-day fold calendar")
def calibration_and_calendar():
    from .daily import CALIBRATION, daily_statistics, sliding_folds
    bars, _ = synthetic_days(n_days=60)
    bars["gap"] = np.log(bars["open"] / bars["close"].shift(1)).fillna(0.0)
    bars["vwap"] = (bars["high"] + bars["low"] + bars["close"]) / 3
    bars["open_ms"] = bars["day"] * 4100.0
    bars["symbol"] = "SYN"
    stats = daily_statistics(bars)
    r = np.log(bars["close"]).diff().dropna()
    exact = (abs(stats["spread_bp"] - 1e4 * 0.002 / 1.0) < 1e-6 and abs(stats["vol_bp"] - 1e4 * r.std()) < 1e-9
             and abs(stats["shares_per_trade"] - (bars["volume"] / bars["trades"]).mean()) < 1e-9 and stats["day_clock_ms"] == 4100.0
             and set(CALIBRATION) <= set(stats))
    folds = sliding_folds(600, 180, 30, 30, 30, embargo=2)
    calendar = len(folds) == 12 and folds[0].test == (234, 264) and folds[-1].test == (564, 594)
    check("calibration: a market's daily statistics, the 600-day fold calendar", exact and calendar,
          f"spread {stats['spread_bp']:.1f} bp, vol {stats['vol_bp']:.0f} bp; {len(folds)} folds, last test {folds[-1].test}")


@guarded("training in parallel processes gives the same agents as serially")
def parallel_training():
    import argparse
    from .daily import sliding_folds
    from .train import train_all
    _, stream = synthetic_days(n_days=120)
    folds = sliding_folds(120, 40, 10, 10, 20, embargo=2)[:2]
    args = argparse.Namespace(hidden=[8], lr=3e-4, n_steps=256, batch_size=64, n_epochs=2, gamma=0.95, clip=0.1, ent_coef=0.01,
                              target_kl=0.025, weight_decay=1e-5, timesteps=512, eval_every=256, dd_penalty=0.5, min_trades=1, jobs=1)
    jobs = [(f, seed) for f in folds for seed in (1, 2)]
    env = {"max_steps": 20}
    serial = train_all(jobs, [stream], args, env)
    args.jobs = 2
    parallel = train_all(jobs, [stream], args, env)
    same = all(np.array_equal(a, b) for x, y in zip(serial, parallel) for a, b in zip(x["agent"].pi.params(), y["agent"].pi.params()))
    same = same and [x["score"] for x in serial] == [y["score"] for y in parallel]
    check("training in parallel processes gives the same agents as serially", same and len(parallel) == 4,
          f"{len(jobs)} trainings, --jobs 1 and --jobs 2: identical weights and checkpoint scores")


@guarded("charts: every plotting function writes its file from a small synthetic run")
def charts():
    from . import plots
    plt = plots.plotting()
    if plt is None:
        results.append(("charts: every plotting function writes its file from a small synthetic run", "SKIP", "matplotlib missing"))
        return
    from .daily import daily_statistics
    rng = np.random.default_rng(5)
    bars, stream = synthetic_days(n_days=60)
    bars = bars.assign(symbol="SYN", gap=np.log(bars["open"] / bars["close"].shift(1)).fillna(0.0), vwap=bars["close"], open_ms=bars["day"] * 4100.0,
                       close_ms=bars["day"] * 4100.0 + 4000.0)
    real = pd.DataFrame([{"ticker": t, "drift_bp": d, "vol_bp": v, "autocorr": a, "range_bp": r, "abs_gap_bp": g, "overnight_share": o}
                         for t, d, v, a, r, g, o in (("A", 4, 120, 0.01, 160, 50, 0.3), ("B", 6, 200, -0.01, 230, 90, 0.45))])
    simulated = pd.DataFrame([daily_statistics(bars)] * 3)
    returns = np.log(bars["close"]).diff().dropna().to_numpy()
    evals = pd.DataFrame([{"fold": f, "seed": sd, "timesteps": t, "train_eval_ret": rng.normal(0, 0.01), "val_ret": rng.normal(0, 0.01),
                           "train_dd": 0.01, "val_dd": 0.01, "score": rng.normal(0, 0.01), "eligible": t > 2048, "entropy": 3.5 - t / 1e4,
                           "approx_kl": 0.01, "explained_variance": 0.1} for f in (1, 2) for sd in (1, 2) for t in (2048, 4096, 6144)])
    folds = pd.DataFrame([{"fold": f, "val_score": float(evals[(evals.fold == f) & evals.eligible].score.max()), "test_return_pct": r,
                           "test_profit_factor": pf, "test_sharpe": sh, "always_long_sharpe": -1.0, "trend_hold_sharpe": 0.5, "random_sharpe": -3.0}
                          for f, r, pf, sh in ((1, 0.01, 1.2, 1.5), (2, -0.01, 0.8, -0.5))])
    oos = pd.DataFrame({"test_day": range(20), "seed_1": 1 + np.cumsum(rng.normal(0, 1e-4, 20)), "seed_2": 1 + np.cumsum(rng.normal(0, 1e-4, 20))})
    baselines = pd.DataFrame([{"baseline": b, "seed": sd, "test_day": d, "equity": 1 + d * 1e-5} for b in ("always_long", "flat") for sd in (1, 2) for d in range(20)])
    actions = pd.DataFrame([{"fold": 1, "held": h, "direction": d, "sl_atr": 1.5, "tp_r": 2.0, "count": 5} for h in (0, 1) for d in (0, 1, -1)])
    trades = pd.DataFrame([{"symbol": "SYN", "direction": 1, "entry_day": d, "exit_day": d + 2, "entry": bars["open"][d], "exit": bars["close"][d + 2],
                            "stop": bars["open"][d] * 0.98, "target": bars["open"][d] * 1.04, "reason": rs, "r": r}
                           for d, rs, r in ((22, "TP", 2.0), (30, "SL", -1.1), (40, "close", 0.3))])
    holdout = {n: {"return_pct": 0.01, "sharpe": 0.5, "profit_factor": 1.1} for n in ("agent", "ensemble", "always_long", "flat")}
    samples = pd.DataFrame([{"elapsed_ms": ms, "client": 7, "net_worth": 1e7 * (1 + ms * 1e-9)} for ms in bars["open_ms"]])
    prices = pd.DataFrame({"elapsed_ms": bars["open_ms"], "symbol": "SYN", "last_price": bars["close"]})
    final = pd.DataFrame([{"client": 7, "label": "rl", "initial_cash": 5e6, "initial_net_worth": 1e7}])
    live = pd.DataFrame([{"seed": 42, "strategy": "rl", "bots": 2, "entries": 12}])
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        written = [
            plots.plot_calibration_stats(plt, simulated, real, out / "a.png"),
            plots.plot_return_distributions(plt, returns, returns[:30], returns, returns[:30], out / "b.png"),
            plots.plot_autocorrelation(plt, [returns, returns[::-1]], [returns], out / "c.png"),
            plots.plot_learning_curves(plt, evals, folds, out / "d.png"),
            plots.plot_ppo_diagnostics(plt, evals, out / "e.png", 3.58),
            plots.plot_fold_table(plt, folds, out / "f.png"),
            plots.plot_oos_equity(plt, oos, baselines, out / "g.png"),
            plots.plot_actions(plt, actions, trades, out / "h.png"),
            plots.plot_sealed(plt, holdout, out / "i.png"),
            plots.plot_rl_equity(plt, samples, prices, final, bars, {"rl"}, out / "j.png"),
            plots.plot_rl_trades_on_candles(plt, bars, trades, out / "k.png"),
            plots.plot_execution(plt, plots.execution_costs(trades, bars), out / "l.png"),
            plots.plot_activity(plt, live, {42: 60}, out / "m.png"),
        ]
        page = plots.index_page(out, "test", "", [("all", [p.name for p in written], "")])
        sizes = [p.stat().st_size if p.exists() else 0 for p in written]
        entry, stop, target = plots.execution_costs(trades, bars)
        ok = all(size > 10_000 for size in sizes) and page.exists() and len(stop) == 1 and abs(stop.iloc[0] - 1e4 * (trades.stop[1] - trades.exit[1]) / trades.stop[1]) < 1e-6
        check("charts: every plotting function writes its file from a small synthetic run", ok,
              f"{len(written)} charts, smallest {min(sizes) // 1000} kB, the index page; the execution costs exact")


@guarded("RL environment: entry at the open, the stop and the target walked through the day")
def environment():
    from .env import RLEnv, rollout
    _, stream = synthetic_days()
    env = RLEnv([stream], (30, 31), {"slippage_bps": 0.0, "stop_slippage_bps": 0.0}, random_start=False, stream_index=0)
    env.reset()
    entry = stream.open[30] * (1 + 0.5 * stream.spread_bps[29] / 1e4)
    # a tight day: force both a stop and a target inside the day's path, the path's order decides
    stream.intraday[30] = np.array([stream.open[30], entry * 0.97, entry * 1.10])
    env.step([1, 0, 0])  # long, stop 1 ATR, target 1 R
    # the path trades through the stop (at -3%, the stop is 1 ATR, about 2%, away) before the target: the stop-loss fills at
    # that trade's price, worse than the stop, like a stop order in a fast market
    sl_hit = bool(env.trades) and env.trades[-1]["reason"] == "SL" and abs(env.trades[-1]["exit"] - entry * 0.97) < 1e-9 and env.trades[-1]["r"] < -1.0
    curve, trades = rollout(lambda o, e: [1, 1, 3], stream, (25, 75))
    ok = bool(sl_hit) and len(curve) == 50 and all(t["reason"] in ("SL", "TP", "close", "flip") for t in trades)
    check("RL environment: entry at the open, the stop and the target walked through the day", ok,
          f"first trade {env.trades[0]['reason'] if env.trades else '-'}, {len(trades)} trades on a drifting walk")


@guarded("our PPO: exact gradients, and it learns to go long in a rising market")
def ppo_learning():
    from .env import HEADS, RLEnv, obs_dim
    from .ppo import PPOAgent, PPOConfig
    rng = np.random.default_rng(0)
    agent = PPOAgent(6, [3, 3, 4], PPOConfig(hidden=(8, 5), seed=1, ent_coef=0.05))
    agent.pi.W[-1] += rng.normal(0, 0.5, agent.pi.W[-1].shape)
    x = rng.normal(size=(7, 6))
    acts = np.stack([rng.integers(0, 3, 7), rng.integers(0, 3, 7), rng.integers(0, 4, 7)], 1)
    old, adv = rng.normal(-3, 0.3, 7), rng.normal(size=7)
    _, grads, _, _, _ = agent.policy_gradients(x, acts, old, adv)
    worst = 0.0
    for p, g in zip(agent.pi.params(), grads):
        idx = tuple(int(rng.integers(0, n)) for n in p.shape)
        o = p[idx]
        p[idx] = o + 1e-6
        up = agent.policy_gradients(x, acts, old, adv)[0]
        p[idx] = o - 1e-6
        down = agent.policy_gradients(x, acts, old, adv)[0]
        p[idx] = o
        worst = max(worst, abs((up - down) / 2e-6 - g[idx]))
    _, stream = synthetic_days(n_days=200, drift=0.004)
    learner = PPOAgent(obs_dim(), HEADS, PPOConfig(hidden=(32, 16), learning_rate=1e-3, n_steps=512, batch_size=128, seed=3, ent_coef=0.01))
    env_rng = np.random.default_rng(1)
    state = {}
    while learner.timesteps < 12_000:
        learner.update(learner.collect(lambda: RLEnv([stream], (25, 200), {}, rng=env_rng), 512, state), 1 - learner.timesteps / 12_000)
    obs = RLEnv([stream], (25, 200), {}, random_start=False, stream_index=0).reset()
    p_long = learner.probabilities(obs)[0][1]
    reloaded = PPOAgent.from_state(learner.state())
    same = reloaded.act(obs, deterministic=True)[0] == learner.act(obs, deterministic=True)[0]
    check("our PPO: exact gradients, and it learns to go long in a rising market", worst < 1e-6 and p_long > 0.5 and same,
          f"largest gradient error {worst:.1e}, P(long) {p_long:.2f} after 12k steps, same action after reload")


@guarded("server: daily bars in, an RL agent's trade out, the gate")
def rl_server():
    from .policy import RLModel
    bars, _ = synthetic_days()
    with tempfile.TemporaryDirectory() as tmp:
        rl_artifact(Path(tmp), "bravo", gate_passed=False)
        refused = load_policies(Path(tmp), log=lambda *_: None)
        served = load_policies(Path(tmp), log=lambda *_: None, allow_ungated=True)
        service = PolicyService(served, log=lambda *_: None)
        answers = [service.answer(f"OBSERVE SYN {row.day} {row.close} {row.bid} {row.ask} {row.bid_depth} {row.ask_depth} {row.volume} {row.open} {row.high} {row.low} {row.trades}")
                   for row in bars.head(25).itertuples()]
        info = service.answer("INFO bravo")
        flat = service.answer("ACT bravo SYN dir=0 entry=0 sl=0 tp=0 days=0 tpr=0")
        held = service.answer("ACT bravo SYN dir=1 entry=101 sl=99 tp=105 days=2 tpr=2")
        ok = (not refused and isinstance(served["bravo"], RLModel) and all(a.startswith("OK OBSERVED") for a in answers)
              and "agent=ppo" in info and "train_seeds=301,302" in info and flat.startswith("OK ACTION") and "sl_atr=" in flat and "atr=" in held)
        check("server: daily bars in, an RL agent's trade out, the gate", ok, flat[:110])

@guarded("server over a real socket: OBSERVE, ACT, INFO, STATUS, MODELS, errors")
def socket_server():
    bars, _ = synthetic_days()
    with tempfile.TemporaryDirectory() as tmp:
        rl_artifact(Path(tmp), "bravo", gate_passed=True)
        srv = make_server(PolicyService(load_policies(Path(tmp), log=lambda *_: None), log=lambda *_: None), 0)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            with socket.create_connection(("127.0.0.1", srv.server_address[1]), timeout=20) as sock:
                def ask(text):
                    send_frame(sock, text)
                    return recv_frame(sock)
                early = ask("ACT bravo SYN dir=0 entry=0 sl=0 tp=0 days=0 tpr=0")
                for row in bars.head(30).itertuples():
                    answer = ask(f"OBSERVE SYN {row.day} {row.close} {row.bid} {row.ask} {row.bid_depth} {row.ask_depth} {row.volume} {row.open} {row.high} {row.low} {row.trades}")
                action = ask("ACT bravo SYN dir=0 entry=0 sl=0 tp=0 days=0 tpr=0")
                status = ask("STATUS bravo")
                ok = (ask("MODELS") == "OK MODELS bravo" and early.startswith("ERR NOT_ENOUGH_DATA") and answer.startswith("OK OBSERVED symbol=SYN n=30")
                      and action.startswith("OK ACTION model=bravo symbol=SYN position=") and "decisions=1" in status
                      and ask("OBSERVE SYN 1 2").startswith("ERR USAGE") and ask("ACT nope SYN").startswith("ERR UNKNOWN_MODEL")
                      and ask("FORECAST bravo SYN").startswith("ERR UNKNOWN_COMMAND") and ask("ACT bravo SYN dir=x").startswith("ERR BAD_REQUEST"))
                check("server over a real socket: OBSERVE, ACT, INFO, STATUS, MODELS, errors", ok, action[:90])
        finally:
            srv.shutdown()
            srv.server_close()


def main() -> None:
    modules_import()
    daily_features()
    calibration_and_calendar()
    parallel_training()
    charts()
    environment()
    ppo_learning()
    rl_server()
    socket_server()
    width = max(len(r[0]) for r in results)
    for name, status, detail in results:
        print(f"[{status:7s}] {name:{width}s}  {detail}")
    failed = sum(1 for r in results if r[1] == "FAIL")
    skipped = sum(1 for r in results if r[1] == "SKIP")
    print(f"\n{len(results) - failed - skipped} passed, {failed} failed" + (f", {skipped} skipped" if skipped else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
