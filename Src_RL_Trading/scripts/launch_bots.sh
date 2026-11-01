#!/usr/bin/env bash
# launch_bots.sh, runs the client-server session: starts sim_server.x, connects N socket bots
# (sim_client.x --bot, each one trading across ALL of the server's symbols through the wire protocol),
# stops the server gracefully so it writes its end-of-run market report, then plots everything
# Everything stays inside the project:
#
#   Src_Simulation/output/<timestamp>/          (Src_Simulation/output/latest points to the newest run)
#     logs/server_bots.log                      server log: every trade, rejection, client command
#     logs/bots/botNN_<strategy>.log            each bot's ORDER / CANCEL commands and the server's answers
#     metrics_bots.csv                          throughput/latency over time
#     summary_bots.csv, symbols_bots.csv, ...   end-of-run market report (see include/market_recorder.hpp)
#     plots/metrics_bots.png, plots/0X_*_bots.png, plots/report_bots.html
#
# Usage (from anywhere, no option is required, the defaults give a complete ~20s session):
#   Src_Simulation/scripts/launch_bots.sh
#   Src_Simulation/scripts/launch_bots.sh --bots 30 --duration 120
#   Src_Simulation/scripts/launch_bots.sh 15 60            # old positional form: [num_bots] [duration] [port]
# To run it together with the in-process simulation, use the global launcher: Src_Simulation/launch.sh
#
# Options:
#   --output-dir DIR       run folder (default: Src_Simulation/output/<YYYYmmdd_HHMMSS>)
#   --bots N               number of socket bots (default 9), strategies assigned round-robin: noise momentum marketmaker meanreversion trend twap, plus ml when Src_Simulation/models/signal_model.csv exists (scripts/train_signal_model.py)
#   --duration SEC         how long each bot trades (default 20)
#   --port N               server port (default 8000)
#   --symbols N            tickers the server loads from the data seed (default 8)
#   --seed N               base RNG seed, bot i uses seed+i (default 1)
#   --refresh-data MODE    small | full | tickers-file: refresh Data/ first (needs network, see refresh_data.sh)
#   --period P             with --refresh-data: how far back to fetch (default 5y)
#   --interval I           with --refresh-data: bar size 1d | 1h | 1wk | 1mo (default 1d)
#   --history-length N     with --refresh-data: keep each ticker's N most recent bars (default 100)
#   --min-rows N           with --refresh-data: drop tickers with fewer bars (default 30)
#   --train-ml             retrain the ML bots' model on the fetched real market first (it is trained anyway when no model exists yet, see scripts/ml_model.sh)
#   --ml-bar-ms N          the ML bots' bar: one simulated trading day of N ms (default 1000), also the candle width of the charts: one candle = one simulated trading day
#   --closing-auction SEC  end the session like a trading day: the last SEC seconds are the pre-close (orders accepted, nothing matches), then the closing auction sets each symbol's close and every order left expires (see sim_server.x --closing-auction) (SEC must be shorter than --duration)
#   --no-plots             write the CSVs and logs but skip the Python plots
#   -h, --help             show this help
#
# Plots need Python 3 with pandas and matplotlib, without them the CSVs are still written and the plotting step is skipped with a message
# The launcher itself connects once as the account "launcher" (to list symbols and to print the final market state), so that account shows up in the report as a client with no trades
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# paths are displayed relative to the project root (e.g. Src_Simulation/output/...), never absolute
ROOT_DIR="$(cd "${SIM_DIR}/.." && pwd)"
rel() { printf '%s' "${1#"${ROOT_DIR}"/}"; }

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

OUTPUT_DIR=""
NUM_BOTS=9
DURATION=20
PORT=8000
SYMBOLS=8
SEED=1
REFRESH_DATA=""
REFRESH_ARGS=()
PLOTS=1
TRAIN_ML=0
ML_BAR_MS=1000
CLOSING_AUCTION=-1

positional=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)   OUTPUT_DIR="$2"; shift 2 ;;
        --bots)         NUM_BOTS="$2"; shift 2 ;;
        --duration)     DURATION="$2"; shift 2 ;;
        --port)         PORT="$2"; shift 2 ;;
        --symbols)      SYMBOLS="$2"; shift 2 ;;
        --seed)         SEED="$2"; shift 2 ;;
        --refresh-data) REFRESH_DATA="$2"; shift 2 ;;
        --period|--interval|--history-length|--history_length|--min-rows|--min_rows)
            REFRESH_ARGS+=("$1" "$2"); shift 2 ;;
        --no-plots)     PLOTS=0; shift ;;
        --train-ml)     TRAIN_ML=1; shift ;;
        --closing-auction) CLOSING_AUCTION="$2"; shift 2 ;;
        --ml-bar-ms)    ML_BAR_MS="$2"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        [0-9]*)         # backward-compatible positional form: [num_bots] [duration] [port]
            case "${positional}" in
                0) NUM_BOTS="$1" ;;
                1) DURATION="$1" ;;
                2) PORT="$1" ;;
                *) echo "Too many positional arguments (see --help)" >&2; exit 1 ;;
            esac
            positional=$((positional + 1)); shift ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done
if [[ "${NUM_BOTS}" -lt 1 ]]; then
    echo "--bots must be at least 1" >&2; exit 1
fi

if [[ -z "${REFRESH_DATA}" && ${#REFRESH_ARGS[@]} -gt 0 ]]; then
    echo "--period/--interval/--history-length/--min-rows only apply together with --refresh-data MODE" >&2
    exit 1
fi

# run folder: default is a fresh timestamped folder inside the project 
if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="${SIM_DIR}/output/$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "${OUTPUT_DIR}/logs/bots"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
if [[ "$(dirname "${OUTPUT_DIR}")" == "${SIM_DIR}/output" ]]; then
    ln -sfn "$(basename "${OUTPUT_DIR}")" "${SIM_DIR}/output/latest"
fi
LOG_DIR="${OUTPUT_DIR}/logs"

if [[ -n "${REFRESH_DATA}" ]]; then
    "${SCRIPT_DIR}/refresh_data.sh" "${REFRESH_DATA}" ${REFRESH_ARGS[@]+"${REFRESH_ARGS[@]}"}
fi

echo "==> Building sim_server.x and sim_client.x"
(cd "${SIM_DIR}" && make sim_server.x sim_client.x > "${LOG_DIR}/build_bots.log" 2>&1) || {
    echo "Build failed, see $(rel "${LOG_DIR}/build_bots.log")" >&2; exit 1; }

cd "${SIM_DIR}"

# server and bots are tracked by PID and always stopped on exit (normal end, error, Ctrl-C) 
SERVER_PID=""
BOT_PIDS=()
stop_server() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2> /dev/null; then
        kill -TERM "${SERVER_PID}" 2> /dev/null || true # graceful: the server writes its report on SIGTERM
        wait "${SERVER_PID}" 2> /dev/null || true
    fi
    SERVER_PID=""
}
cleanup() {
    for pid in ${BOT_PIDS[@]+"${BOT_PIDS[@]}"}; do kill "${pid}" 2> /dev/null || true; done
    stop_server
}
trap cleanup EXIT
trap 'echo "interrupted, stopping bots and server" >&2; exit 130' INT TERM

echo "==> Starting sim_server.x on port ${PORT} (log: $(rel "${LOG_DIR}/server_bots.log"))"
CLOSING_ARGS=()
if [[ "${CLOSING_AUCTION}" -ge 0 ]]; then
    if [[ "${CLOSING_AUCTION}" -ge "${DURATION}" ]]; then
        echo "--closing-auction ${CLOSING_AUCTION} must be shorter than --duration ${DURATION}" >&2; exit 1
    fi
    CLOSING_ARGS=(--closing-auction "${CLOSING_AUCTION}")
fi
./sim_server.x --port "${PORT}" --max-symbols "${SYMBOLS}" --output-dir "${OUTPUT_DIR}" ${CLOSING_ARGS[@]+"${CLOSING_ARGS[@]}"} > "${LOG_DIR}/server_bots.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 40); do
    grep -q "listening on port" "${LOG_DIR}/server_bots.log" 2> /dev/null && break
    kill -0 "${SERVER_PID}" 2> /dev/null || break
    sleep 0.25
done
if ! grep -q "listening on port" "${LOG_DIR}/server_bots.log" 2> /dev/null; then
    echo "Server failed to start (port ${PORT} already in use?), see $(rel "${LOG_DIR}/server_bots.log")" >&2
    exit 1
fi

# one helper account for the launcher's own queries (registered once, then reused with LOGIN)
SYMBOLS_LINE="$(printf 'SYMBOLS\n' | ./sim_client.x launcher launcher_pw --register --port "${PORT}" | grep '^OK SYMBOLS ' || true)"
SERVER_SYMBOLS=(${SYMBOLS_LINE#OK SYMBOLS })
echo "==> Server symbols (every bot trades across all of them): ${SERVER_SYMBOLS[*]:-none}"

# every strategy from bot.hpp, assigned round-robin, the ML bot joins when a trained model exists
STRATEGIES=(noise momentum marketmaker meanreversion trend twap)
ML_MODEL="${SIM_DIR}/models/signal_model.csv"
source "${SCRIPT_DIR}/ml_model.sh"
ensure_signal_model "${TRAIN_ML}" || true # trained from the fetched market if missing, without data no ml bot
if [[ -f "${ML_MODEL}" ]]; then
    STRATEGIES+=(ml)
fi
echo "==> Launching ${NUM_BOTS} bots for ${DURATION}s, strategies round-robin: ${STRATEGIES[*]}"
for i in $(seq 1 "${NUM_BOTS}"); do
    strategy="${STRATEGIES[$(( (i - 1) % ${#STRATEGIES[@]} ))]}"
    # the account name carries the strategy, so the report can group P&L by strategy
    # One decision every 100-400 ms: the history-based strategies (meanreversion, trend, ml) need 15 to 20 price samples before their first order, too many for a short session at sim_client.x's slower default
    name="bot$(printf '%02d' "${i}")_${strategy}"
    ./sim_client.x "${name}" "pw${i}" --register --port "${PORT}" --bot "${strategy}" \
        --duration-sec "${DURATION}" --seed "$((SEED + i))" --ml-model "${ML_MODEL}" --ml-bar-ms "${ML_BAR_MS}" \
        --min-interval-ms 100 --max-interval-ms 400 \
        > "${LOG_DIR}/bots/${name}.log" 2>&1 &
    BOT_PIDS+=($!)
done
# the end of the trading day: the server is told to close CLOSING_AUCTION seconds before the bots stop, so their last orders form the pre-close and meet in the closing auction
if [[ "${CLOSING_AUCTION}" -ge 0 ]]; then
    (sleep "$((DURATION - CLOSING_AUCTION))" && kill -TERM "${SERVER_PID}" 2> /dev/null) &
    echo "==> Pre-close in $((DURATION - CLOSING_AUCTION))s: ${CLOSING_AUCTION}s of order collection, then the closing auction"
fi
wait "${BOT_PIDS[@]}" || true

if [[ "${CLOSING_AUCTION}" -ge 0 ]]; then
    echo "==> Final market state: the server closed the trading day (closing prices below, once it has stopped)"
else
    echo "==> Final market state:"
    {
        echo "METRICS"
        for symbol in "${SERVER_SYMBOLS[@]:-}"; do
            if [[ -n "${symbol}" ]]; then echo "MARKET ${symbol}"; fi
        done
    } | ./sim_client.x launcher launcher_pw --port "${PORT}" | grep -E '^OK (METRICS|MARKET)' || true
fi

echo "==> Stopping the server (it writes the market report on shutdown)"
stop_server

if [[ ! -f "${OUTPUT_DIR}/summary_bots.csv" ]]; then
    echo "The market report was not written, see $(rel "${LOG_DIR}/server_bots.log")" >&2
    exit 1
fi
echo "    trades logged: $(grep -c ' TRADE ' "${LOG_DIR}/server_bots.log" || true) (grep TRADE $(rel "${LOG_DIR}/server_bots.log"))"
if [[ "${CLOSING_AUCTION}" -ge 0 ]]; then
    echo "==> End of the trading day:"
    grep -E "^Pre-close|^  close |order\(s\) expired" "${LOG_DIR}/server_bots.log" | sed 's/^/    /' || true
fi

if [[ "${PLOTS}" -eq 1 ]]; then
    if python3 -c "import pandas, matplotlib" > /dev/null 2>&1; then
        echo "==> Plotting (metrics, then market report)"
        python3 "${SCRIPT_DIR}/plot_metrics.py" "${OUTPUT_DIR}" --suffix bots || echo "metrics plot failed (CSV is fine)" >&2
        python3 "${SCRIPT_DIR}/plot_market_report.py" "${OUTPUT_DIR}" --candle-seconds "$(awk "BEGIN {print ${ML_BAR_MS} / 1000}")" --suffix bots > "${LOG_DIR}/plot_report_bots.log" 2>&1 && echo "    report: $(rel "${OUTPUT_DIR}/plots/report_bots.html")" || { echo "market report plot failed, see $(rel "${LOG_DIR}/plot_report_bots.log")" >&2; }
    else
        echo "==> pandas/matplotlib not installed: skipping plots (pip install pandas matplotlib)"
    fi
fi

echo "==> Done: $(rel "${OUTPUT_DIR}")"
