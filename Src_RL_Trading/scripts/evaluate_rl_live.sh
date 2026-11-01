#!/usr/bin/env bash
# evaluate_rl_live.sh: the live verdict on the RL agents, inside simulation.x, on market seeds none of them ever saw: 
# every served agent trades as its own RL bots (RLBot: entry at the open, a GTC stop-loss and take-profit) among the rule-based bots, and the run reports what each strategy added over holding its starting account
#
#   Src_RL_Trading/output/rl_eval_<timestamp>/seed_<S>/   each market's report, its RL trades, the server's log
#   Src_RL_Trading/output/rl_eval_<timestamp>/summary.csv one row per strategy per seed
#   Src_RL_Trading/output/rl_eval_<timestamp>/index.html    the verdict page and its charts (plots/), each market's report in seed_<S>/plots/
#
# One policy server per market (a server keeps one stream per symbol: two markets sharing it would mix their days), all the markets at once (each run sleeps most of the time)
#
# Usage (from anywhere):
#   Src_RL_Trading/scripts/evaluate_rl_live.sh                       # the ensemble, the production model and the two rule baselines, 5 markets of 60 days (seeds 42..46)
#   Src_RL_Trading/scripts/evaluate_rl_live.sh --models "rl rl_always_long" --seeds "42 43" --days 120
#
# Options:
#   --models "M1 M2"   the agents (default: rl's ensemble members, rl, rl_always_long, rl_trend_hold)
#   --seeds "S1 S2"    market seeds (default "42 43 44 45 46")
#   --days N           trading days per market (default 60)
#   --rl-bots N        bots per agent (default 2)
#   --first-port N     the servers' ports: N, N+1, ... (default 7990)
#   -h, --help         show this help
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${SIM_DIR}/.." && pwd)"
rel() { printf '%s' "${1#"${ROOT_DIR}"/}"; }
usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }
source "${SCRIPT_DIR}/policy_server.sh"

MODELS=""
SEEDS="42 43 44 45 46"
DAYS=60
RL_BOTS=2
FIRST_PORT=7990
while [[ $# -gt 0 ]]; do
    case "$1" in
        --models)     MODELS="$2"; shift 2 ;;
        --seeds)      SEEDS="$2"; shift 2 ;;
        --days)       DAYS="$2"; shift 2 ;;
        --rl-bots)    RL_BOTS="$2"; shift 2 ;;
        --first-port) FIRST_PORT="$2"; shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done
ARTIFACTS="$(policy_dir)/artifacts"
if [[ -z "${MODELS}" ]]; then
    MODELS="$(policy_rl_models)"
    for extra in rl rl_always_long rl_trend_hold; do
        [[ -f "${ARTIFACTS}/${extra}/metadata.json" && " ${MODELS} " != *" ${extra} "* ]] && MODELS="${MODELS} ${extra}"
    done
fi
[[ -n "${MODELS// /}" ]] || { echo "no RL agent in $(rel "${ARTIFACTS}"): python3 -m rl_agents.train first" >&2; exit 1; }
OUT="${SIM_DIR}/output/rl_eval_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${OUT}"
(cd "${SIM_DIR}" && make simulation.x > /dev/null)
echo "==> ${MODELS} on seeds ${SEEDS}, ${DAYS} trading days each, into $(rel "${OUT}")"

SERVERS=()
RUNS=()
cleanup() { for pid in ${SERVERS[@]+"${SERVERS[@]}"}; do kill "${pid}" 2> /dev/null || true; done; }
trap cleanup EXIT
port="${FIRST_PORT}"
for seed in ${SEEDS}; do
    run="${OUT}/seed_${seed}"
    mkdir -p "${run}"
    while lsof -nP -iTCP:"${port}" -sTCP:LISTEN > /dev/null 2>&1; do port=$((port + 1)); done # a port nobody listens on
    # shellcheck disable=SC2086
    (cd "$(policy_dir)" && exec python3 -m rl_agents.serve --port "${port}" --models ${MODELS} --allow-ungated) > "${run}/policy_server.log" 2>&1 &
    SERVERS+=($!)
    for _ in $(seq 1 120); do grep -q "listening on port" "${run}/policy_server.log" 2> /dev/null && break; sleep 0.25; done
    (cd "${SIM_DIR}" && ./simulation.x --days "${DAYS}" --value 40 --marketmakers 12 --maker-half-spread 0.001 --symbols 5 --seed "${seed}" --log-level ERROR \
        --rl-port "${port}" --rl-bots "${RL_BOTS}" --output-dir "${run}" > "${run}/simulation.log" 2>&1) &
    RUNS+=($!)
    port=$((port + 1))
done
wait "${RUNS[@]}" 2> /dev/null || true # the markets, then the servers go (cleanup)
cleanup

python3 - "${OUT}" << 'PY'
import re, sys
from pathlib import Path
import pandas as pd
rows = []
for log in sorted(Path(sys.argv[1]).glob("seed_*/simulation.log")):
    seed = int(log.parent.name.split("_")[1])
    for line in log.read_text().splitlines():
        m = re.match(r"^(\S+): bots=(\d+) mean_pnl=(\S+) mean_excess_vs_holding=(\S+)", line)
        if m:
            rows.append({"seed": seed, "strategy": m[1], "bots": int(m[2]), "excess": float(m[4])})
        b = re.match(r"^(\S+) rl: .*entries=(\d+) closed=(\d+) mean_r=(\S+) win_rate=(\S+) exits:(.*) waited", line)
        if b:
            for r in rows[::-1]:
                if r["seed"] == seed and r["strategy"] == b[1]:
                    r.update(entries=int(b[2]), closed=int(b[3]), mean_r=float(b[4]), win_rate=float(b[5]), exits=b[6].strip())
df = pd.DataFrame(rows)
df.to_csv(Path(sys.argv[1]) / "summary.csv", index=False)
table = df.groupby("strategy").agg(mean_excess=("excess", "mean"), seeds_positive=("excess", lambda x: int((x > 0).sum())), seeds=("excess", "size"))
if "mean_r" in df:
    table = table.join(df.groupby("strategy").agg(trades=("closed", "sum"), mean_r=("mean_r", "mean"), win_rate=("win_rate", "mean")))
pd.set_option("display.width", 200)
# the value investors see each company's hidden fundamental value: an informed benchmark, apart, never ranked with the strategies that only see prices
informed = table.index.isin(["value"])
print("strategies that only see prices, ranked by mean excess over holding:")
print(table[~informed].sort_values("mean_excess", ascending=False).round(3).to_string())
if informed.any():
    print("\ninformed benchmark (sees the hidden fundamental value), not ranked:")
    print(table[informed].round(3).to_string())
PY
# the charts: every market's report, then the verdict page (scripts/plot_rl_report.py: the gate, the folds, the live results, the RL bots over the days, their trades, their execution against the training environment)
if python3 -c "import matplotlib" > /dev/null 2>&1; then
    for run in "${OUT}"/seed_*; do
        (python3 "${SCRIPT_DIR}/plot_market_report.py" "${run}" --suffix simu > "${run}/plot_report.log" 2>&1 || echo "market report of $(basename "${run}") failed" >&2) &
    done
    wait
    python3 "${SCRIPT_DIR}/plot_rl_report.py" "${OUT}" > "${OUT}/plot_rl_report.log" 2>&1 && echo "==> Charts: $(rel "${OUT}/index.html")" \
        || echo "the verdict page failed, see $(rel "${OUT}/plot_rl_report.log")" >&2
else
    echo "==> matplotlib not installed: no charts (pip install matplotlib)"
fi
echo "==> Done: $(rel "${OUT}/summary.csv")"
