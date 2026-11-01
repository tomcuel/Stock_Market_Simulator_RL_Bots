#!/usr/bin/env bash
# launch_simulation.sh, runs the in-process simulation (simulation.x), keeps its log, and plots
# its metrics and end-of-run market report. Everything stays inside the project:
#
#   Src_Simulation/output/<timestamp>/          (Src_Simulation/output/latest points to the newest run)
#     logs/simulation_simu.log                  full log (every trade at the default INFO level)
#     metrics_simu.csv                          throughput/latency over time
#     summary_simu.csv, symbols_simu.csv, ...   end-of-run market report (see include/market_recorder.hpp)
#     plots/metrics_simu.png, plots/0X_*_simu.png, plots/report_simu.html
#
# Usage (from anywhere, no option is required, the defaults give a complete ~20s run):
#   Src_Simulation/scripts/launch_simulation.sh
#   Src_Simulation/scripts/launch_simulation.sh --duration 300 --symbols 8 --noise 60 --momentum 20 --marketmakers 12
#   Src_Simulation/scripts/launch_simulation.sh --refresh-data small     # re-download real data first
# To run it together with the socket-bot session, use the global launcher: Src_Simulation/launch.sh
#
# Options:
#   --output-dir DIR       run folder (default: Src_Simulation/output/<YYYYmmdd_HHMMSS>)
#   --duration SEC         simulation length (default 40: the ML bots need about 35 one-second bars, see MLSignalBot)
#   --symbols N            symbols to simulate (default 5)
#   --noise N              noise-trader bots (default 15)
#   --momentum N           momentum bots (default 5)
#   --marketmakers N       market-maker bots (default 4)
#   --meanreversion N      mean-reversion bots (default 4)
#   --trend N              trend-following bots (default 4)
#   --twap N               TWAP execution bots (default 2)
#   --ml N                 machine-learning bots (default 2, only if the model exists, see scripts/train_signal_model.py)
#   --seed N               RNG seed (default 42)
#   --log-level LEVEL      DEBUG | INFO | WARN | ERROR (default INFO)
#   --refresh-data MODE    small | full | tickers-file: refresh Data/ first (needs network, see refresh_data.sh)
#   --period P             with --refresh-data: how far back to fetch (default 5y)
#   --interval I           with --refresh-data: bar size 1d | 1h | 1wk | 1mo (default 1d)
#   --history-length N     with --refresh-data: keep each ticker's N most recent bars (default 100)
#   --min-rows N           with --refresh-data: drop tickers with fewer bars (default 30)
#   --train-ml             retrain the ML bots' model on the fetched real market first (it is trained anyway when --ml N > 0 and no model exists yet, see scripts/ml_model.sh)
#   --ml-bar-ms N          the ML bots' bar: one simulated trading day of N ms (default 1000), also the candle width of the charts: one candle = one simulated trading day
#   --no-plots             write the CSVs and logs but skip the Python plots
#   -h, --help             show this help
#
# Plots need Python 3 with pandas and matplotlib, without them the CSVs are still written and the
# plotting step is skipped with a message.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# paths are displayed relative to the project root (e.g. Src_Simulation/output/...), never absolute
ROOT_DIR="$(cd "${SIM_DIR}/.." && pwd)"
rel() { printf '%s' "${1#"${ROOT_DIR}"/}"; }

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

OUTPUT_DIR=""
DURATION=40
SYMBOLS=5
NOISE=15
MOMENTUM=5
MARKETMAKERS=4
MEANREVERSION=4
TREND=4
TWAP=2
ML=2
TRAIN_ML=0
ML_BAR_MS=1000
SEED=42
LOG_LEVEL="INFO"
REFRESH_DATA=""
REFRESH_ARGS=()
PLOTS=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)   OUTPUT_DIR="$2"; shift 2 ;;
        --duration)     DURATION="$2"; shift 2 ;;
        --symbols)      SYMBOLS="$2"; shift 2 ;;
        --noise)        NOISE="$2"; shift 2 ;;
        --momentum)     MOMENTUM="$2"; shift 2 ;;
        --marketmakers) MARKETMAKERS="$2"; shift 2 ;;
        --meanreversion) MEANREVERSION="$2"; shift 2 ;;
        --trend)        TREND="$2"; shift 2 ;;
        --twap)         TWAP="$2"; shift 2 ;;
        --ml)           ML="$2"; shift 2 ;;
        --train-ml)     TRAIN_ML=1; shift ;;
        --ml-bar-ms)    ML_BAR_MS="$2"; shift 2 ;;
        --seed)         SEED="$2"; shift 2 ;;
        --log-level)    LOG_LEVEL="$2"; shift 2 ;;
        --refresh-data) REFRESH_DATA="$2"; shift 2 ;;
        --period|--interval|--history-length|--history_length|--min-rows|--min_rows)
            REFRESH_ARGS+=("$1" "$2"); shift 2 ;;
        --no-plots)     PLOTS=0; shift ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done

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

if [[ "${ML}" -gt 0 ]]; then
    source "${SCRIPT_DIR}/ml_model.sh"
    ensure_signal_model "${TRAIN_ML}" || true # without data the ML bots just don't trade
fi

echo "==> Building simulation.x"
(cd "${SIM_DIR}" && make simulation.x > "${OUTPUT_DIR}/logs/build_simu.log" 2>&1) || {
    echo "Build failed, see $(rel "${OUTPUT_DIR}/logs/build_simu.log")" >&2; exit 1; }

LOG_FILE="${OUTPUT_DIR}/logs/simulation_simu.log"
echo "==> Running simulation.x for ${DURATION}s on ${SYMBOLS} symbols (bots: ${NOISE} noise, ${MOMENTUM} momentum, ${MARKETMAKERS} market maker, ${MEANREVERSION} mean reversion, ${TREND} trend, ${TWAP} twap, ${ML} ml)"
echo "    log: $(rel "${LOG_FILE}")"
if ! (cd "${SIM_DIR}" && ./simulation.x --duration "${DURATION}" --symbols "${SYMBOLS}" --noise "${NOISE}" --momentum "${MOMENTUM}" --marketmakers "${MARKETMAKERS}" --meanreversion "${MEANREVERSION}" --trend "${TREND}" --twap "${TWAP}" --ml "${ML}" --ml-bar-ms "${ML_BAR_MS}" --seed "${SEED}" --log-level "${LOG_LEVEL}" --output-dir "${OUTPUT_DIR}" > "${LOG_FILE}" 2>&1); then
    echo "simulation.x failed, last lines of $(rel "${LOG_FILE}"):" >&2
    tail -20 "${LOG_FILE}" >&2
    exit 1
fi
sed -n '/=== Simulation report/,$p' "${LOG_FILE}"

if [[ "${PLOTS}" -eq 1 ]]; then
    if python3 -c "import pandas, matplotlib" > /dev/null 2>&1; then
        echo "==> Plotting (metrics, then market report)"
        python3 "${SCRIPT_DIR}/plot_metrics.py" "${OUTPUT_DIR}" --suffix simu || echo "metrics plot failed (CSV is fine)" >&2
        python3 "${SCRIPT_DIR}/plot_market_report.py" "${OUTPUT_DIR}" --candle-seconds "$(awk "BEGIN {print ${ML_BAR_MS} / 1000}")" --suffix simu > "${OUTPUT_DIR}/logs/plot_report_simu.log" 2>&1 && echo "    report: $(rel "${OUTPUT_DIR}/plots/report_simu.html")" || { echo "market report plot failed, see $(rel "${OUTPUT_DIR}/logs/plot_report_simu.log")" >&2; }
    else
        echo "==> pandas/matplotlib not installed: skipping plots (pip install pandas matplotlib)"
    fi
fi

echo "==> Done: $(rel "${OUTPUT_DIR}")"
