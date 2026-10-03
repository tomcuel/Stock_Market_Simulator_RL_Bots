#!/usr/bin/env bash
# launch.sh: global launcher for Src_Simulation: runs, into one run folder inside the project:
#
#   simu phase:  simulation.x (in-process, bigger/faster)  -> plot_metrics.py -> plot_market_report.py
#   bots phase:  sim_server.x + socket bots (smaller, through the real wire protocol)-> plot_metrics.py -> plot_market_report.py
# then writes plots/index.html linking both reports: files from the two phases never collide: everything from the simulation ends in _simu, everything from the server/bots ends in _bots.
#
#   Src_Simulation/output/<timestamp>/          (Src_Simulation/output/latest points to the newest run)
#     logs/          simulation_simu.log, server_bots.log, bots/botNN_<strategy>.log, build/plot logs
#     *_simu.csv     metrics + market report of the simulation
#     *_bots.csv     metrics + market report of the server/bots session
#     plots/         metrics_{simu,bots}.png, 0X_*_{simu,bots}.png, report_{simu,bots}.html, index.html
#
# Usage (from anywhere; no option is required: the defaults run both phases, ~45s in total):
#   Src_Simulation/launch.sh                  # both phases (same as: launch.sh all)
#   Src_Simulation/launch.sh simu             # only the in-process simulation
#   Src_Simulation/launch.sh bots             # only the server + socket bots session
#   Src_Simulation/launch.sh simu --simu-duration 600 --symbols 8 --noise 60 --momentum 20 --marketmakers 12
#   Src_Simulation/launch.sh all --refresh-data small    # re-download real market data first
#   Src_Simulation/launch.sh simu --refresh-data full --period 10y --history-length 250
# Then open Src_Simulation/output/latest/plots/index.html
#
# Options:
#   (first argument)       which phase to run: all | simu | bots (default: all)
#   --output-dir DIR       run folder (default: Src_Simulation/output/<YYYYmmdd_HHMMSS>)
#   --duration SEC         length of BOTH phases (default: simu 20, bots 20)
#   --simu-duration SEC    length of the simulation phase only
#   --bots-duration SEC    length of the bots phase only
#   --symbols N            symbols in the simulation, and tickers loaded by the server (default: 5 / 8)
#   --seed N               RNG seed for both phases (default: 42 / 1)
#   --noise N              simulation: noise-trader bots (default 15)
#   --momentum N           simulation: momentum bots (default 5)
#   --marketmakers N       simulation: market-maker bots (default 4)
#   --bots N               bots phase: number of socket bots (default 9)
#   --port N               bots phase: server port (default 8000)
#   --refresh-data MODE    small | full | tickers-file: refresh Data/ once before running (needs network, see scripts/refresh_data.sh)
#   --period P             with --refresh-data: how far back to fetch (default 5y)
#   --interval I           with --refresh-data: bar size 1d | 1h | 1wk | 1mo (default 1d)
#   --history-length N     with --refresh-data: keep each ticker's N most recent bars (default 100)
#   --min-rows N           with --refresh-data: drop tickers with fewer bars (default 30)
#   --no-plots             write CSVs and logs only, skip the Python plots
#   -h, --help             show this help
#
# Each phase can also be run on its own with more options: scripts/launch_simulation.sh and scripts/launch_bots.sh (see their headers)
set -euo pipefail

SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_DIR="${SIM_DIR}/scripts"

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

MODE="all"
if [[ $# -gt 0 ]]; then
    case "$1" in
        all|simu|bots) MODE="$1"; shift ;;
        -h|--help) usage; exit 0 ;;
    esac
fi

OUTPUT_DIR=""
REFRESH_DATA=""
REFRESH_ARGS=()
SIMU_ARGS=()
BOTS_ARGS=()
COMMON_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)    OUTPUT_DIR="$2"; shift 2 ;;
        --refresh-data)  REFRESH_DATA="$2"; shift 2 ;;
        --period|--interval|--history-length|--history_length|--min-rows|--min_rows)
            REFRESH_ARGS+=("$1" "$2"); shift 2 ;;
        --duration)      SIMU_ARGS+=(--duration "$2"); BOTS_ARGS+=(--duration "$2"); shift 2 ;;
        --simu-duration) SIMU_ARGS+=(--duration "$2"); shift 2 ;;
        --bots-duration) BOTS_ARGS+=(--duration "$2"); shift 2 ;;
        --symbols)       SIMU_ARGS+=(--symbols "$2"); BOTS_ARGS+=(--symbols "$2"); shift 2 ;;
        --seed)          SIMU_ARGS+=(--seed "$2"); BOTS_ARGS+=(--seed "$2"); shift 2 ;;
        --noise|--momentum|--marketmakers) SIMU_ARGS+=("$1" "$2"); shift 2 ;;
        --bots|--port)   BOTS_ARGS+=("$1" "$2"); shift 2 ;;
        --no-plots)      COMMON_ARGS+=(--no-plots); shift ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
done

if [[ -z "${REFRESH_DATA}" && ${#REFRESH_ARGS[@]} -gt 0 ]]; then
    echo "--period/--interval/--history-length/--min-rows only apply together with --refresh-data MODE" >&2
    exit 1
fi
if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="${SIM_DIR}/output/$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

if [[ -n "${REFRESH_DATA}" ]]; then
    "${SCRIPT_DIR}/refresh_data.sh" "${REFRESH_DATA}" ${REFRESH_ARGS[@]+"${REFRESH_ARGS[@]}"}
fi

echo "################ run folder: ${OUTPUT_DIR}"
if [[ "${MODE}" == "all" || "${MODE}" == "simu" ]]; then
    echo "################ phase 1: in-process simulation (simulation.x)"
    "${SCRIPT_DIR}/launch_simulation.sh" --output-dir "${OUTPUT_DIR}" ${SIMU_ARGS[@]+"${SIMU_ARGS[@]}"} ${COMMON_ARGS[@]+"${COMMON_ARGS[@]}"}
fi
if [[ "${MODE}" == "all" || "${MODE}" == "bots" ]]; then
    echo "################ phase 2: server + socket bots (sim_server.x + sim_client.x --bot)"
    "${SCRIPT_DIR}/launch_bots.sh" --output-dir "${OUTPUT_DIR}" ${BOTS_ARGS[@]+"${BOTS_ARGS[@]}"} ${COMMON_ARGS[@]+"${COMMON_ARGS[@]}"}
fi

# one page linking everything this run produced
PLOTS_DIR="${OUTPUT_DIR}/plots"
if [[ -d "${PLOTS_DIR}" ]]; then
    {
        echo '<!doctype html><html><head><meta charset="utf-8"><title>Run '"$(basename "${OUTPUT_DIR}")"'</title>'
        echo '<style>body{background:#131722;color:#d1d4dc;font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;margin:24px}'
        echo 'a{color:#42a5f5}h2{border-bottom:1px solid #2a2e39;padding-bottom:6px}img{max-width:100%;border:1px solid #2a2e39;border-radius:6px}</style></head><body>'
        echo "<h1>Run $(basename "${OUTPUT_DIR}")</h1>"
        for sfx in simu bots; do
            if [[ -f "${PLOTS_DIR}/report_${sfx}.html" ]]; then
                label="in-process simulation"; [[ "${sfx}" == "bots" ]] && label="server + socket bots"
                echo "<h2>${label} (_${sfx})</h2><p><a href=\"report_${sfx}.html\">full market report</a></p>"
                [[ -f "${PLOTS_DIR}/metrics_${sfx}.png" ]] && echo "<img src=\"metrics_${sfx}.png\"/>"
                [[ -f "${PLOTS_DIR}/01_price_paths_${sfx}.png" ]] && echo "<img src=\"01_price_paths_${sfx}.png\"/>"
            fi
        done
        echo '</body></html>'
    } > "${PLOTS_DIR}/index.html"
fi

if [[ "$(dirname "${OUTPUT_DIR}")" == "${SIM_DIR}/output" ]]; then
    ln -sfn "$(basename "${OUTPUT_DIR}")" "${SIM_DIR}/output/latest"
fi
echo "################ done: ${OUTPUT_DIR}"
[[ -f "${PLOTS_DIR}/index.html" ]] && echo "open ${PLOTS_DIR}/index.html"
exit 0
