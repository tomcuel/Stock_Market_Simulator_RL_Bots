"""
Policy server: a live, streaming service in front of the trained RL agents, what the C++ simulator's RLBots ask

Usage (from RL_Agents/):
    python -m rl_agents.serve                            # every gated agent of artifacts/, port 7979
    python -m rl_agents.serve --models rl_f4 rl_f5 --port 8000
    python -m rl_agents.serve --models rl rl_always_long --allow-ungated

Protocol (Src_RL_Trading's framing: an 8-byte big-endian length, then the text):
    OBSERVE <symbol> <day> <close> <bid> <ask> <bid_depth> <ask_depth> <volume> <open> <high> <low> <trades>
                                    -> OK OBSERVED symbol=AAPL n=61        (one daily bar, pushed by the C++ MarketFeed at every close: the book before the pre-close, the day's volume, open, high, low, trades)
    ACT <model> <symbol> dir=.. entry=.. sl=.. tp=.. days=.. tpr=..
                                    -> OK ACTION model=rl symbol=AAPL position=d direction=d sl_atr=.. tp_r=.. atr=.. (the bot's open trade in, the next trade out, see policy.py)
    MODELS                          -> OK MODELS rl_f4 rl_f5 ...
    INFO <model>                    -> OK INFO model=.. agent=ppo|rule lookback=<days> train_seeds=<the simulation seeds it learned from, comma separated, -1 if none>
    STATUS <model>                  -> OK STATUS model=.. observations=.. decisions=.. long=.. flat=.. short=.. latency_ms=..
    QUIT
ACT answers ERR NOT_ENOUGH_DATA until the symbol's stream holds the agent's lookback (its features' warm-up days).
Every connection has its own thread.
"""
from __future__ import annotations

import argparse
import itertools
import signal
import socketserver
import sys
import threading
from collections import deque
from pathlib import Path

import pandas as pd

from .paths import display_path
from .policy import RLModel, RLServed
from .protocol import recv_frame, send_frame

ROOT = Path(__file__).resolve().parent.parent

# the columns of an OBSERVE, in order (after the symbol and the day)
STREAM_COLS = ["close", "bid", "ask", "bid_depth", "ask_depth", "volume", "open", "high", "low", "trades"]


class StreamStore:
    """
    One rolling buffer of daily bars per symbol, filled by OBSERVE: what the agents' observations are built from
    """
    def __init__(self, max_rows: int = 20000):
        self.max_rows = max_rows
        self.rows: dict[str, deque] = {}
        self.counts: dict[str, int] = {} # total observations ever received per symbol
        self.lock = threading.Lock()

    def append(self, symbol: str, time: float, values: list[float]) -> int:
        with self.lock:
            buffer = self.rows.setdefault(symbol, deque(maxlen=self.max_rows))
            buffer.append([time] + list(values))
            self.counts[symbol] = self.counts.get(symbol, 0) + 1
            return self.counts[symbol]

    def frame(self, symbol: str, last: int | None = None) -> pd.DataFrame:
        with self.lock:
            buffer = self.rows.get(symbol, deque())
            if last and last < len(buffer):
                rows = list(itertools.islice(buffer, len(buffer) - last, len(buffer))) # only the tail
            else:
                rows = list(buffer)
        return pd.DataFrame(rows, columns=["time"] + STREAM_COLS)

    def count(self, symbol: str) -> int:
        with self.lock:
            return self.counts.get(symbol, 0)


def load_policies(artifacts: Path, only: list[str] | None = None, log=print, allow_ungated: bool = False) -> dict:
    policies = {}
    folders = sorted(p for p in Path(artifacts).iterdir() if (p / "metadata.json").exists()) if Path(artifacts).is_dir() else []
    for folder in folders:
        if only and folder.name not in only:
            continue
        if not RLModel.is_rl(folder):
            log(f"skipping {folder.name}: not an RL agent (agent ppo or rule)")
            continue
        model = RLModel.load_any(folder)
        if not model.gate_passed and not allow_ungated:
            # the walk-forward gate (train.py) did not approve it: not served unless asked
            log(f"skipping {folder.name}: it failed the walk-forward consistency gate (serve it anyway with --allow-ungated)")
            continue
        policies[folder.name] = model
    return policies


def _fmt(value) -> str:
    return f"{value:.8g}" if isinstance(value, float) else str(value)


class PolicyService:
    def __init__(self, policies: dict, log=print):
        self.store = StreamStore()
        self.log = log
        self.served = {name: RLServed(name, model, self.store, log) for name, model in policies.items()}

    def answer(self, line: str) -> str:
        tokens = line.split()
        if not tokens:
            return "ERR EMPTY"
        command = tokens[0].upper()
        if command == "OBSERVE":
            if len(tokens) != 13:
                return "ERR USAGE OBSERVE <symbol> <day> <close> <bid> <ask> <bid_depth> <ask_depth> <volume> <open> <high> <low> <trades>"
            try:
                values = [float(v) for v in tokens[2:]]
            except ValueError:
                return "ERR BAD_REQUEST non-numeric value"
            if values[1] <= 0:
                return "ERR BAD_REQUEST close must be positive"
            n = self.store.append(tokens[1], values[0], values[1:])
            for served in self.served.values():
                served.on_observe(tokens[1])
            return f"OK OBSERVED symbol={tokens[1]} n={n}"
        if command == "MODELS":
            return "OK MODELS " + " ".join(self.served)
        if command in ("INFO", "ACT", "STATUS"):
            if len(tokens) < 2:
                return f"ERR USAGE {command} <model>"
            if tokens[1] not in self.served:
                return f"ERR UNKNOWN_MODEL {tokens[1]}"
            served = self.served[tokens[1]]
            model = served.model
            if command == "INFO":
                seeds = ",".join(str(s) for s in model.provenance.get("seeds", [])) or "-1"
                return f"OK INFO model={tokens[1]} agent={model.type_name} lookback={model.lookback} train_seeds={seeds}"
            if command == "STATUS":
                return f"OK STATUS model={tokens[1]} " + " ".join(f"{k}={_fmt(v)}" for k, v in served.stats.items())
            if len(tokens) < 3:
                return "ERR USAGE ACT <model> <symbol> dir=.. entry=.. sl=.. tp=.. days=.. tpr=.."
            have = self.store.count(tokens[2])
            if have < model.lookback:
                return f"ERR NOT_ENOUGH_DATA have={have} need={model.lookback}"
            try:
                state = {}
                for token in tokens[3:]:
                    key, _, value = token.partition("=")
                    state[key] = float(value)
                out = served.act(tokens[2], state)
            except ValueError as error:
                return f"ERR BAD_REQUEST {str(error).replace(' ', '_')}"
            return f"OK ACTION model={tokens[1]} symbol={tokens[2]} " + " ".join(f"{k}={_fmt(v)}" for k, v in out.items())
        return f"ERR UNKNOWN_COMMAND {command}"


def make_server(service: PolicyService, port: int) -> socketserver.ThreadingTCPServer:
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            while True:
                try:
                    line = recv_frame(self.request)
                except OSError:
                    return
                if line is None or line.strip().upper() == "QUIT":
                    return
                try:
                    reply = service.answer(line)
                except Exception as error: # one bad request must never take the server down
                    reply = f"ERR INTERNAL {type(error).__name__}"
                try:
                    send_frame(self.request, reply)
                except OSError:
                    return

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    server = socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    return server


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifacts", type=Path, default=ROOT / "artifacts")
    parser.add_argument("--port", type=int, default=7979)
    parser.add_argument("--models", nargs="*", default=None)
    parser.add_argument("--allow-ungated", action="store_true", help="also serve agents that failed the walk-forward gate")
    args = parser.parse_args()

    def log(message):
        print(message, file=sys.stderr, flush=True)

    policies = load_policies(args.artifacts, args.models, log, args.allow_ungated)
    if not policies:
        raise SystemExit(f"no agent to serve in {display_path(args.artifacts)}: train one first (python3 -m rl_agents.train, see Src_RL_Trading/README.md)")
    for name, m in policies.items():
        print(f"  {name}: {m.type_name}, needs {m.lookback} days")
    server = make_server(PolicyService(policies, log=log), args.port)
    def stop(*_):
        raise KeyboardInterrupt # a launcher's kill (SIGTERM) stops the server like Ctrl-C
    signal.signal(signal.SIGTERM, stop)
    print(f"policy server listening on port {args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
