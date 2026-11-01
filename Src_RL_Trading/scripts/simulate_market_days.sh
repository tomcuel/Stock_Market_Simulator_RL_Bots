#!/usr/bin/env bash
# simulate_market_days.sh: records the market the RL agent trains in: N runs of simulation.x with trading days (each
# seed its own market: the same rules and bots, other news and order flow), the rule-based bots only, no RL bot
#
#   Src_RL_Trading/RL_Agents/sim_days/<market>/seed_<S>/   daily_bars_simu.csv (one bar per symbol per day), trades_simu.csv...
#
# The default market is the calibrated "real" one (README, "A real market architecture"): 
# - 5 real tickers from their last real price
# - 40 value investors trading toward each company's fundamental value (a random walk on the ticker's real volatility, 35% of its variance arriving overnight)
# - 12 market makers quoting mid +-0.1%, the usual noise, momentum, mean-reversion, trend and TWAP bots. Its days move like the real tickers' (about 1.2-1.4% a day against 1.1 to 2.3%, a 13 bp spread, gaps at the open)
# The agent then trains with python -m rl_agents.train --data sim_days/<market>
#
# Scale (measured on an 8-core M1, README section 2): a market uses about 2.5% of a core whatever the number running at once (80 at once: 2 cores, the same wall time and the same daily statistics as 12), so all the markets run together.
# Memory and disk grow with the days: the recorder keeps the run's trades and samples until it ends, about 60-70 MB per 600-day run with --report-interval-ms 1000 (the portfolio samples, which training never reads, were 2/3 of the files at the default 250 ms). 
# At the end, scripts/market_calibration.py prints the market's daily statistics against real5_d600, the reference market (unless it is the one just recorded: then over time only)
#
# Usage (from anywhere):
#   Src_RL_Trading/scripts/simulate_market_days.sh                         # 40 markets of 600 days (seeds 401..440), ~42 min
#   Src_RL_Trading/scripts/simulate_market_days.sh --runs 4 --days 60      # 4 markets of 60 days (the 240 s of launch.sh)
#
# Options:
#   --market NAME          folder name under RL_Agents/sim_days/ (default real5_d600; real5 is the earlier 12 x 250 days)
#   --runs N               number of markets (default 40)
#   --first-seed S         seeds S, S+1, ... (default 401: not the judging seeds 42..)
#   --days N               trading days per market (default 600: 12 walk-forward folds of 180/30/30 days, see train.py)
#   --day-ms N             one trading day (default 4000: shorter days thin the book, the bots tick every 20-200 ms)
#   --parallel N           markets recorded at the same time (default 40: each run sleeps most of the time, ~2.5% of a core)
#   --report-interval-ms N the book and portfolio sampling period (default 1000; training reads the daily bars and the trades)
#   --symbols N, --noise N, --momentum N, --marketmakers N, --meanreversion N, --trend N, --twap N, --value N, --maker-half-spread X  # the market, as simulation.x takes them (defaults: 5, 15, 5, 12, 4, 4, 2, 40, 0.001)
#   -h, --help             show this help
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${SIM_DIR}/.." && pwd)"
rel() { printf '%s' "${1#"${ROOT_DIR}"/}"; }
usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

MARKET="real5_d600"
RUNS=40
FIRST_SEED=401
DAYS=600
DAY_MS=4000
PARALLEL=40
REPORT_MS=1000
MARKET_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --market)       MARKET="$2"; shift 2 ;;
        --runs)         RUNS="$2"; shift 2 ;;
        --first-seed)   FIRST_SEED="$2"; shift 2 ;;
        --days)         DAYS="$2"; shift 2 ;;
        --day-ms)       DAY_MS="$2"; shift 2 ;;
        --parallel)     PARALLEL="$2"; shift 2 ;;
        --report-interval-ms) REPORT_MS="$2"; shift 2 ;;
        --symbols|--noise|--momentum|--marketmakers|--meanreversion|--trend|--twap|--value|--maker-half-spread) MARKET_ARGS+=("$1" "$2"); shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done
# the calibrated market, unless overridden
for pair in "--symbols 5" "--marketmakers 12" "--value 40" "--maker-half-spread 0.001"; do
    flag="${pair%% *}"
    [[ " ${MARKET_ARGS[*]-} " == *" ${flag} "* ]] || MARKET_ARGS+=(${pair})
done

# one line per finished market: its days, trades and how its daily closes moved
summarize() {
    awk -F, -v seed="$1" 'NR > 1 {n++; t += $11; if ($1 > d) d = $1} END {printf "    seed %s: %d days, %.0f trades per symbol per day\n", seed, d + 1, t / n}' "$2/daily_bars_simu.csv"
}

OUT_DIR="${SIM_DIR}/RL_Agents/sim_days/${MARKET}"
mkdir -p "${OUT_DIR}"
echo "==> Building simulation.x"
(cd "${SIM_DIR}" && make simulation.x > /dev/null)
echo "==> ${RUNS} markets of ${DAYS} days of ${DAY_MS} ms into $(rel "${OUT_DIR}") (seeds ${FIRST_SEED} to $((FIRST_SEED + RUNS - 1)), ${PARALLEL} at a time): ${MARKET_ARGS[*]}"
running=0
for i in $(seq 0 $((RUNS - 1))); do
    seed=$((FIRST_SEED + i))
    run="${OUT_DIR}/seed_${seed}"
    if [[ -f "${run}/daily_bars_simu.csv" ]]; then
        echo "    seed ${seed}: already there, kept"
        continue
    fi
    (cd "${SIM_DIR}" && ./simulation.x --days "${DAYS}" --day-ms "${DAY_MS}" --seed "${seed}" --log-level ERROR --report-interval-ms "${REPORT_MS}" --output-dir "${run}" "${MARKET_ARGS[@]}" \
        > "${run}.log" 2>&1 && summarize "${seed}" "${run}") &
    running=$((running + 1))
    if [[ "${running}" -ge "${PARALLEL}" ]]; then
        wait # a batch at a time (macOS's bash 3.2 has no wait -n)
        running=0
    fi
done
wait
echo "==> Done: $(ls -d "${OUT_DIR}"/seed_* 2> /dev/null | grep -vc '\.log$') markets in $(rel "${OUT_DIR}")"
BASELINE="${SIM_DIR}/RL_Agents/sim_days/real5_d600" # the reference market
echo "==> Daily statistics (scripts/market_calibration.py)"
if python3 -c "import pandas" > /dev/null 2>&1; then
    python3 "${SCRIPT_DIR}/market_calibration.py" "${OUT_DIR}" $([[ -d "${BASELINE}" && "${BASELINE}" != "${OUT_DIR}" ]] && echo --baseline "${BASELINE}") \
        || echo "==> the recorded market failed a calibration check (above): look at it before training on it" >&2
    # the calibration charts against the real tickers (rl_agents.calibration): RL_Agents/reports/<market>/plots/index.html
    (cd "${SIM_DIR}/RL_Agents" && python3 -m rl_agents.calibration --data "sim_days/${MARKET}") || echo "    (calibration charts failed, the recording is fine)" >&2
else
    echo "    (calibration skipped: python3 with pandas needed)" >&2
fi
