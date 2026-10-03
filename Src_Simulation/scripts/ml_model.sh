#!/usr/bin/env bash
# ml_model.sh: helper sourced by launch_simulation.sh and launch_bots.sh, not meant to be run directly
# The ML bots trade with models/signal_model.csv, trained by scripts/train_signal_model.py on the real market fetched by the Data/ pipeline (no previous simulation run needed)
#   ensure_signal_model FORCE   trains the model if FORCE is 1 or if it doesn't exist yet, from Data/Datasets/processed/all_prices.csv (or features.csv) without fetched data, says how to get it and returns 1 (the ML bots then simply don't trade)

ensure_signal_model() {
    local force="$1"
    local sim_dir root_dir model processed
    sim_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    root_dir="$(cd "${sim_dir}/.." && pwd)"
    model="${sim_dir}/models/signal_model.csv"
    processed="${root_dir}/Data/Datasets/processed"
    if [[ "${force}" -ne 1 && -f "${model}" ]]; then
        return 0
    fi
    if [[ ! -f "${processed}/all_prices.csv" && ! -f "${processed}/features.csv" ]]; then
        echo "==> No fetched market data to train the ML bots on: run scripts/refresh_data.sh small (or launch with --refresh-data small)" >&2
        return 1
    fi
    echo "==> Training the ML bots' model on the fetched real market (scripts/train_signal_model.py)"
    python3 "${sim_dir}/scripts/train_signal_model.py" | sed 's/^/    /'
}
