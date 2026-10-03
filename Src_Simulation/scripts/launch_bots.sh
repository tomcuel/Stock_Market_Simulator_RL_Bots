#!/usr/bin/env bash
# launch_bots.sh: runs the client-server session: starts sim_server.x, connects N socket bots (sim_client.x --bot, each one trading across ALL of the server's symbols through the wire protocol),
# stops the server gracefully so it writes its end-of-run market report, then plots everything: everything stays inside the project:
#
#   Src_Simulation/output/<timestamp>/          (Src_Simulation/output/latest points to the newest run)
#     logs/server_bots.log                      server log: every trade, rejection, client command
#     logs/bots/botNN_<strategy>.log            each bot's orders and the server's answers
#     metrics_bots.csv                          throughput/latency over time
#     summary_bots.csv, symbols_bots.csv, ...   end-of-run market report (see include/market_recorder.hpp)
#     plots/metrics_bots.png, plots/0X_*_bots.png, plots/report_bots.html
#
# Usage (from anywhere; no option is required: the defaults give a complete ~20s session):
#   Src_Simulation/scripts/launch_bots.sh
#   Src_Simulation/scripts/launch_bots.sh --bots 30 --duration 120
#   Src_Simulation/scripts/launch_bots.sh 15 60            # old positional form: [num_bots] [duration] [port]
# To run it together with the in-process simulation, use the global launcher: Src_Simulation/launch.sh
#
# Options:
#   --output-dir DIR       run folder (default: Src_Simulation/output/<YYYYmmdd_HHMMSS>)
#   --bots N               number of socket bots, strategies assigned round-robin: noise / momentum / marketmaker (default 9)
#   --duration SEC         how long each bot trades (default 20)
#   --port N               server port (default 8000)
#   --symbols N            tickers the server loads from the data seed (default 8)
#   --seed N               base RNG seed, bot i uses seed+i (default 1)
#   --refresh-data MODE    small | full | tickers-file: refresh Data/ first (needs network, see refresh_data.sh)
#   --period P             with --refresh-data: how far back to fetch (default 5y)
#   --interval I           with --refresh-data: bar size 1d | 1h | 1wk | 1mo (default 1d)
#   --history-length N     with --refresh-data: keep each ticker's N most recent bars (default 100)
#   --min-rows N           with --refresh-data: drop tickers with fewer bars (default 30)
#   --no-plots             write the CSVs and logs but skip the Python plots
#   -h, --help             show this help
#
# The launcher itself connects once as the account "launcher" (to list symbols and to print the final market state), so that account shows up in the report as a client with no trades
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

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
    echo "Build failed: see ${LOG_DIR}/build_bots.log" >&2; exit 1; }

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
trap 'echo "interrupted: stopping bots and server" >&2; exit 130' INT TERM

echo "==> Starting sim_server.x on port ${PORT} (log: ${LOG_DIR}/server_bots.log)"
./sim_server.x --port "${PORT}" --max-symbols "${SYMBOLS}" --output-dir "${OUTPUT_DIR}" > "${LOG_DIR}/server_bots.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 40); do
    grep -q "listening on port" "${LOG_DIR}/server_bots.log" 2> /dev/null && break
    kill -0 "${SERVER_PID}" 2> /dev/null || break
    sleep 0.25
done
if ! grep -q "listening on port" "${LOG_DIR}/server_bots.log" 2> /dev/null; then
    echo "Server failed to start (port ${PORT} already in use?): see ${LOG_DIR}/server_bots.log" >&2
    exit 1
fi

# one helper account for the launcher's own queries (registered once, then reused with LOGIN)
SYMBOLS_LINE="$(printf 'SYMBOLS\n' | ./sim_client.x launcher launcher_pw --register --port "${PORT}" | grep '^OK SYMBOLS ' || true)"
SERVER_SYMBOLS=(${SYMBOLS_LINE#OK SYMBOLS })
echo "==> Server symbols (every bot trades across all of them): ${SERVER_SYMBOLS[*]:-none}"

STRATEGIES=(noise momentum marketmaker)
echo "==> Launching ${NUM_BOTS} bots for ${DURATION}s"
for i in $(seq 1 "${NUM_BOTS}"); do
    strategy="${STRATEGIES[$(( (i - 1) % 3 ))]}"
    ./sim_client.x "bot${i}" "pw${i}" --register --port "${PORT}" --bot "${strategy}" --duration-sec "${DURATION}" --seed "$((SEED + i))" > "${LOG_DIR}/bots/bot$(printf '%02d' "${i}")_${strategy}.log" 2>&1 &
    BOT_PIDS+=($!)
done
wait "${BOT_PIDS[@]}" || true

echo "==> Final market state:"
{
    echo "METRICS"
    for symbol in "${SERVER_SYMBOLS[@]:-}"; do
        if [[ -n "${symbol}" ]]; then echo "MARKET ${symbol}"; fi
    done
} | ./sim_client.x launcher launcher_pw --port "${PORT}" | grep -E '^OK (METRICS|MARKET)' || true

echo "==> Stopping the server (it writes the market report on shutdown)"
stop_server

if [[ ! -f "${OUTPUT_DIR}/summary_bots.csv" ]]; then
    echo "The market report was not written: see ${LOG_DIR}/server_bots.log" >&2
    exit 1
fi
echo "    trades logged: $(grep -c ' TRADE ' "${LOG_DIR}/server_bots.log" || true) (grep TRADE ${LOG_DIR}/server_bots.log)"

if [[ "${PLOTS}" -eq 1 ]]; then
    if python3 -c "import pandas, matplotlib" > /dev/null 2>&1; then
        echo "==> Plotting (metrics, then market report)"
        python3 "${SCRIPT_DIR}/plot_metrics.py" "${OUTPUT_DIR}" --suffix bots || echo "metrics plot failed (CSV is fine)" >&2
        python3 "${SCRIPT_DIR}/plot_market_report.py" "${OUTPUT_DIR}" --suffix bots > "${LOG_DIR}/plot_report_bots.log" 2>&1 \
            && echo "    report: ${OUTPUT_DIR}/plots/report_bots.html" \
            || { echo "market report plot failed: see ${LOG_DIR}/plot_report_bots.log" >&2; }
    else
        echo "==> pandas/matplotlib not installed: skipping plots (pip install pandas matplotlib)"
    fi
fi

echo "==> Done: ${OUTPUT_DIR}"
