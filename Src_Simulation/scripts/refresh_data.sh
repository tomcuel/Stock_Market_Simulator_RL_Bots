#!/usr/bin/env bash
# refresh_data.sh: re-downloads real market data and rebuilds the seed file the simulator reads (Data/Datasets/processed/latest_snapshot.csv), by running the Data/ pipeline:
#   fetch_data.py -> preprocess.py -> feature_engineering.py
# simulation.x and sim_server.x pick the new tickers/prices/volatility up automatically on their next start
# Called by launch.sh, launch_simulation.sh and launch_bots.sh when given --refresh-data MODE (with the same tuning options below): can also be run on its own
# Same tuning as Src_SQL/scripts/launch_multi_client.sh --reload_prices (--period, --interval, --history_length), which feeds the SQL exchange instead
#
# Usage (from anywhere; no argument is required -- the default refreshes Data/tickers.txt, 5y of daily bars):
#   Src_Simulation/scripts/refresh_data.sh                      # tickers-file mode, 5y, 1d, last 100 rows
#   Src_Simulation/scripts/refresh_data.sh small                # 10 large US stocks
#   Src_Simulation/scripts/refresh_data.sh full --period 10y --history-length 250
#   Src_Simulation/scripts/refresh_data.sh tickers-file --period 1y --interval 1h
#   Src_Simulation/scripts/refresh_data.sh small --dry-run      # print the pipeline commands, run nothing
#
# Arguments and options:
#   MODE                  which tickers to fetch (default tickers-file):
#                           small         10 large US stocks        (fetch_data.py --small-tickers)
#                           full          ~24 world indices         (fetch_data.py --full-tickers)
#                           tickers-file  one ticker per line in --tickers-file
#   --period P            how far back to fetch: 1d 5d 1mo 3mo 6mo 1y 2y 5y 10y ytd max (default 5y)
#   --interval I          bar size: 1d 1h 1wk 1mo (default 1d) (Yahoo serves hourly (1h) bars for the last ~730 days only, so 1h needs --period 2y or shorter)
#   --history-length N    keep only each ticker's N most recent bars (preprocess.py --max-rows, default 100), also accepted as --max_rows
#   --min-rows N          drop tickers with fewer than N bars before that cut (preprocess.py --min-rows, default 30), also accepted as --min_rows
#   --tickers-file PATH   ticker list for tickers-file mode (default Data/tickers.txt)
#   --dry-run             print the commands that would run, without fetching anything
#   -h, --help            show this help
#
# Without a refresh, the simulator reuses whatever latest_snapshot.csv already exists, or falls back to built-in demo tickers if there is none
# The seed's volatility is a 20-bar rolling figure that needs at least 5 bars: with a shorter history the simulator uses a flat 20% instead
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$(cd "${SCRIPT_DIR}/../../Data" && pwd)"

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

MODE="tickers-file"
PERIOD="5y"
INTERVAL="1d"
HISTORY_LENGTH=100
MIN_ROWS=30
TICKERS_FILE="${DATA_DIR}/tickers.txt"
DRY_RUN=0

need_value() { # $1 = option name, $2 = number of remaining args
    if [[ "$2" -lt 2 ]]; then echo "$1 needs a value (see --help)" >&2; exit 1; fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        small|full|tickers-file) MODE="$1"; shift ;;
        --period)                          need_value "$1" $#; PERIOD="$2"; shift 2 ;;
        --interval)                        need_value "$1" $#; INTERVAL="$2"; shift 2 ;;
        --history-length|--history_length) need_value "$1" $#; HISTORY_LENGTH="$2"; shift 2 ;;
        --min-rows|--min_rows)             need_value "$1" $#; MIN_ROWS="$2"; shift 2 ;;
        --tickers-file|--tickers_file)     need_value "$1" $#; TICKERS_FILE="$2"; shift 2 ;;
        --dry-run)                         DRY_RUN=1; shift ;;
        -h|--help)                         usage; exit 0 ;;
        *) echo "Unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
done

# validate everything before touching the network (these mirror fetch_data.py's own choices)
case "${PERIOD}" in
    1d|5d|1mo|3mo|6mo|1y|2y|5y|10y|ytd|max) ;;
    *) echo "Invalid --period '${PERIOD}' (expected 1d 5d 1mo 3mo 6mo 1y 2y 5y 10y ytd max)" >&2; exit 1 ;;
esac
case "${INTERVAL}" in
    1d|1h|1wk|1mo) ;;
    *) echo "Invalid --interval '${INTERVAL}' (expected 1d 1h 1wk 1mo)" >&2; exit 1 ;;
esac
if [[ "${INTERVAL}" == "1h" ]]; then
    case "${PERIOD}" in
        5y|10y|max) echo "--interval 1h only covers ~730 days on Yahoo Finance: use --period 2y or shorter" >&2; exit 1 ;;
    esac
fi
for pair in "--history-length:${HISTORY_LENGTH}" "--min-rows:${MIN_ROWS}"; do
    name="${pair%%:*}"; value="${pair#*:}"
    if ! [[ "${value}" =~ ^[0-9]+$ ]] || [[ "${value}" -le 0 ]]; then
        echo "${name} must be a positive integer, got '${value}'" >&2; exit 1
    fi
done
if [[ "${HISTORY_LENGTH}" -lt 5 ]]; then
    echo "note: --history-length ${HISTORY_LENGTH} < 5 bars: no volatility can be computed, the simulator will use a flat 20%" >&2
fi

case "${MODE}" in
    small) FETCH_ARGS=(--small-tickers) ;;
    full)  FETCH_ARGS=(--full-tickers) ;;
    tickers-file)
        if [[ ! -f "${TICKERS_FILE}" ]]; then
            echo "Ticker list not found: ${TICKERS_FILE}" >&2; exit 1
        fi
        TICKERS_FILE="$(cd "$(dirname "${TICKERS_FILE}")" && pwd)/$(basename "${TICKERS_FILE}")"
        FETCH_ARGS=(--tickers-file "${TICKERS_FILE}") ;;
esac

RUN_NAME="launcher_${MODE}" # -> Data/Datasets/raw/launcher_<mode>_fetched_data.csv, overwritten on each refresh
FETCH_CMD=(python3 fetch_data.py "${FETCH_ARGS[@]}" --period "${PERIOD}" --interval "${INTERVAL}" --output "${RUN_NAME}")
PREPROCESS_CMD=(python3 preprocess.py --keep_only_fetched --input "${RUN_NAME}" --min-rows "${MIN_ROWS}" --max-rows "${HISTORY_LENGTH}")
FEATURES_CMD=(python3 feature_engineering.py)

echo "==> Refreshing market data: mode=${MODE} period=${PERIOD} interval=${INTERVAL} history=${HISTORY_LENGTH} bars (min ${MIN_ROWS})"
if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "    (dry run, in ${DATA_DIR})"
    echo "    ${FETCH_CMD[*]}"
    echo "    ${PREPROCESS_CMD[*]}"
    echo "    ${FEATURES_CMD[*]}"
    exit 0
fi
(cd "${DATA_DIR}" && "${FETCH_CMD[@]}")
(cd "${DATA_DIR}" && "${PREPROCESS_CMD[@]}")
(cd "${DATA_DIR}" && "${FEATURES_CMD[@]}")
echo "==> Seed file updated: ${DATA_DIR}/Datasets/processed/latest_snapshot.csv"
