#!/usr/bin/env bash
# launch_simulation.sh, runs the in-process simulation (simulation.x), keeps its log, and plots its metrics and end-of-run market report. Everything stays inside the project:
#
#   Src_RL_Trading/output/<timestamp>/          (Src_RL_Trading/output/latest points to the newest run)
#     logs/simulation_simu.log                  full log (every trade at the default INFO level)
#     metrics_simu.csv                          throughput/latency over time
#     summary_simu.csv, symbols_simu.csv, ...   end-of-run market report (see include/market_recorder.hpp)
#     daily_bars_simu.csv, rl_trades_simu.csv   the trading days' bars, the RL bots' trades
#     plots/metrics_simu.png, plots/0X_*_simu.png, plots/report_simu.html
#
# By default the market is the calibrated real one (README, "A real market architecture"): 
# 60 trading days of 4 s (each with a pre-open, an opening auction, continuous trading, a pre-close, a closing auction and a night), 40 value investors trading toward each company's fundamental value, 12 market makers quoting mid +-0.1%. 
# The RL agents served are the walk-forward RL agent's ensemble (RL_Agents/artifacts/rl_f*), which decide once per day, when they failed their gate they are still served, and the launcher says so. 
# Without a trained agent the run has the rule-based bots only
#
# Usage (from anywhere, no option is required, the defaults give a complete ~4 min run):
#   Src_RL_Trading/scripts/launch_simulation.sh
#   Src_RL_Trading/scripts/launch_simulation.sh --days 250 --symbols 8      # a trading year, 8 symbols
#   Src_RL_Trading/scripts/launch_simulation.sh --refresh-data small        # re-download real data first
# To run it together with the socket-bot session, use the global launcher: Src_RL_Trading/launch.sh
#
# Options:
#   --output-dir DIR       run folder (default: Src_RL_Trading/output/<YYYYmmdd_HHMMSS>)
#   --days N               trading days (default 60, at least 1)
#   --day-ms N             one trading day (default 4000)
#   --value N              value investors (default 40; 0: no fundamental values)
#   --symbols N            symbols to simulate (default 5)
#   --noise N              noise-trader bots (default 15)
#   --momentum N           momentum bots (default 5)
#   --marketmakers N       market-maker bots (default 12)
#   --maker-half-spread X  the market makers quote mid -/+ X (default 0.001)
#   --meanreversion N      mean-reversion bots (default 4)
#   --trend N              trend-following bots (default 4)
#   --twap N               TWAP execution bots (default 2)
#   --seed N               RNG seed (default 42)
#   --log-level LEVEL      DEBUG | INFO | WARN | ERROR (default INFO)
#   --refresh-data MODE    small | full | tickers-file: refresh Data/ first (needs network, see refresh_data.sh)
#   --period P             with --refresh-data: how far back to fetch (default 5y)
#   --interval I           with --refresh-data: bar size 1d | 1h | 1wk | 1mo (default 1d)
#   --history-length N     with --refresh-data: keep each ticker's N most recent bars (default 100)
#   --min-rows N           with --refresh-data: drop tickers with fewer bars (default 30)
#   --no-rl                no RL bots: only the rule-based strategies (by default the policy server is started)
#   --rl-port N            policy server port (default 7979)
#   --rl-bots N            bots per served agent (default 2)
#   --rl-models "M1 M2"    the agents to serve (default: rl's ensemble members), served even if they failed their gate
#   --no-plots             write the CSVs and logs but skip the Python plots
#   -h, --help             show this help
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# paths are displayed relative to the project root (e.g. Src_RL_Trading/output/...), never absolute
ROOT_DIR="$(cd "${SIM_DIR}/.." && pwd)"
rel() { printf '%s' "${1#"${ROOT_DIR}"/}"; }

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

OUTPUT_DIR=""
DAYS=60
DAY_MS=4000
VALUE=40
SYMBOLS=5
NOISE=15
MOMENTUM=5
MARKETMAKERS=12
MAKER_HALF_SPREAD=0.001
MEANREVERSION=4
TREND=4
TWAP=2
SEED=42
LOG_LEVEL="INFO"
REFRESH_DATA=""
REFRESH_ARGS=()
PLOTS=1
RL=1
RL_PORT=7979
RL_BOTS=2
RL_MODELS=""
NAMED_MODELS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)   OUTPUT_DIR="$2"; shift 2 ;;
        --days)         DAYS="$2"; shift 2 ;;
        --day-ms)       DAY_MS="$2"; shift 2 ;;
        --value)        VALUE="$2"; shift 2 ;;
        --maker-half-spread) MAKER_HALF_SPREAD="$2"; shift 2 ;;
        --symbols)      SYMBOLS="$2"; shift 2 ;;
        --noise)        NOISE="$2"; shift 2 ;;
        --momentum)     MOMENTUM="$2"; shift 2 ;;
        --marketmakers) MARKETMAKERS="$2"; shift 2 ;;
        --meanreversion) MEANREVERSION="$2"; shift 2 ;;
        --trend)        TREND="$2"; shift 2 ;;
        --twap)         TWAP="$2"; shift 2 ;;
        --seed)         SEED="$2"; shift 2 ;;
        --log-level)    LOG_LEVEL="$2"; shift 2 ;;
        --refresh-data) REFRESH_DATA="$2"; shift 2 ;;
        --period|--interval|--history-length|--history_length|--min-rows|--min_rows)
            REFRESH_ARGS+=("$1" "$2"); shift 2 ;;
        --no-plots)     PLOTS=0; shift ;;
        --no-rl)        RL=0; shift ;;
        --rl-port)      RL_PORT="$2"; shift 2 ;;
        --rl-bots)      RL_BOTS="$2"; shift 2 ;;
        --rl-models)    RL_MODELS="$2"; NAMED_MODELS=1; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done

if ! [[ "${DAYS}" =~ ^[0-9]+$ ]] || [[ "${DAYS}" -lt 1 ]]; then
    echo "--days must be at least 1: trading days are the only mode" >&2
    exit 1
fi
if [[ -z "${REFRESH_DATA}" && ${#REFRESH_ARGS[@]} -gt 0 ]]; then
    echo "--period/--interval/--history-length/--min-rows only apply together with --refresh-data MODE" >&2
    exit 1
fi

# run folder: default is a fresh timestamped folder inside the project
if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="${SIM_DIR}/output/$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "${OUTPUT_DIR}/logs"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
if [[ "$(dirname "${OUTPUT_DIR}")" == "${SIM_DIR}/output" ]]; then
    ln -sfn "$(basename "${OUTPUT_DIR}")" "${SIM_DIR}/output/latest"
fi

if [[ -n "${REFRESH_DATA}" ]]; then
    "${SCRIPT_DIR}/refresh_data.sh" "${REFRESH_DATA}" ${REFRESH_ARGS[@]+"${REFRESH_ARGS[@]}"}
fi

echo "==> Building simulation.x"
(cd "${SIM_DIR}" && make simulation.x > "${OUTPUT_DIR}/logs/build_simu.log" 2>&1) || {
    echo "Build failed, see $(rel "${OUTPUT_DIR}/logs/build_simu.log")" >&2; exit 1; }

# the RL agents as extra bots (see scripts/policy_server.sh): the Python policy server, fed each day's bars at the close
RL_ARGS=()
RL_LABEL=""
if [[ "${RL}" -eq 1 ]]; then
    source "${SCRIPT_DIR}/policy_server.sh"
    trap policy_stop EXIT
    if [[ -z "${RL_MODELS}" ]]; then
        RL_MODELS="$(policy_rl_models)"
        if [[ -n "${RL_MODELS}" && "$(policy_rl_gate)" != "passed" ]]; then
            # shown anyway, for what they are: the walk-forward gate refused them (README, section 4)
            echo "==> serving ${RL_MODELS}: they FAILED their walk-forward consistency gate (no edge that holds fold after fold), shown, not approved"
            NAMED_MODELS=1
        fi
    fi
    if policy_start "${OUTPUT_DIR}/logs" "${RL_PORT}" "${RL_MODELS}" "${NAMED_MODELS}"; then
        RL_ARGS=(--rl-port "${RL_PORT}" --rl-bots "${RL_BOTS}")
        RL_LABEL=", ${RL_BOTS} per RL agent"
        # never evaluate an agent on a market it learned from: if a served agent was trained on a market with this seed,
        # this run moves to another seed (simulation.x would refuse its bots)
        TRAINED_SEEDS="$(policy_training_seeds)"
        if [[ " ${TRAINED_SEEDS} " == *" ${SEED} "* ]]; then
            NEW_SEED=$((SEED + 1000))
            while [[ " ${TRAINED_SEEDS} " == *" ${NEW_SEED} "* ]]; do NEW_SEED=$((NEW_SEED + 1000)); done
            echo "==> seed ${SEED} is the seed a served agent was trained on: this run uses seed ${NEW_SEED} (no data leakage)"
            SEED="${NEW_SEED}"
        fi
    fi
fi

LOG_FILE="${OUTPUT_DIR}/logs/simulation_simu.log"
CANDLE_MS=$((DAY_MS + 100)) # only used without daily bars: with trading days the report draws one candle per day itself
echo "==> Running simulation.x for ${DAYS} trading days of ${DAY_MS} ms on ${SYMBOLS} symbols (bots: ${NOISE} noise, ${MOMENTUM} momentum, ${MARKETMAKERS} market maker, ${MEANREVERSION} mean reversion, ${TREND} trend, ${TWAP} twap, ${VALUE} value${RL_LABEL})"
echo "    log: $(rel "${LOG_FILE}")"
if ! (cd "${SIM_DIR}" && ./simulation.x --days "${DAYS}" --day-ms "${DAY_MS}" --value "${VALUE}" --maker-half-spread "${MAKER_HALF_SPREAD}" --symbols "${SYMBOLS}" --noise "${NOISE}" --momentum "${MOMENTUM}" --marketmakers "${MARKETMAKERS}" --meanreversion "${MEANREVERSION}" --trend "${TREND}" --twap "${TWAP}" --seed "${SEED}" --log-level "${LOG_LEVEL}" --output-dir "${OUTPUT_DIR}" ${RL_ARGS[@]+"${RL_ARGS[@]}"} > "${LOG_FILE}" 2>&1); then
    echo "simulation.x failed, last lines of $(rel "${LOG_FILE}"):" >&2
    tail -20 "${LOG_FILE}" >&2
    exit 1
fi
sed -n '/=== Simulation report/,$p' "${LOG_FILE}"

if [[ "${PLOTS}" -eq 1 ]]; then
    if python3 -c "import pandas, matplotlib" > /dev/null 2>&1; then
        echo "==> Plotting (metrics, then market report)"
        python3 "${SCRIPT_DIR}/plot_metrics.py" "${OUTPUT_DIR}" --suffix simu || echo "metrics plot failed (CSV is fine)" >&2
        python3 "${SCRIPT_DIR}/plot_market_report.py" "${OUTPUT_DIR}" --candle-seconds "$(awk "BEGIN {print ${CANDLE_MS} / 1000}")" --suffix simu > "${OUTPUT_DIR}/logs/plot_report_simu.log" 2>&1 && echo "    report: $(rel "${OUTPUT_DIR}/plots/report_simu.html")" || { echo "market report plot failed, see $(rel "${OUTPUT_DIR}/logs/plot_report_simu.log")" >&2; }
    else
        echo "==> pandas/matplotlib not installed: skipping plots (pip install pandas matplotlib)"
    fi
fi

echo "==> Done: $(rel "${OUTPUT_DIR}")"
