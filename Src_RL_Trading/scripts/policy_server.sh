#!/usr/bin/env bash
# policy_server.sh: helpers sourced by the launchers for their RL bots 
# They start the policy server (RL_Agents/rl_agents/serve.py) in the background, list the agents it serves, and stop it
#
#   policy_start LOG_DIR PORT [MODELS] [UNGATED]
#                                 starts the server, logs to LOG_DIR/policy_server.log, waits until it listens (30s at most)
#                                 Serves MODELS (space separated) if given, else every artifact that passed its gate
#                                 UNGATED=1 also serves agents that failed their walk-forward gate (the launchers pass it when the agents were named with --rl-models)
#                                 Sets POLICY_PID and POLICY_MODELS (the served agents)
#                                 Returns 1 if there is no agent or it could not start
#   policy_stop                   stops it (safe to call twice)
#   policy_training_seeds         the seeds of the simulations the trained agents learned from
#   policy_rl_models              the agents to serve by default: the ensemble members of rl (or rl itself)
#   policy_rl_gate                "passed" if rl passed its walk-forward gate, "failed" otherwise

POLICY_PID=""
POLICY_MODELS=""

policy_dir() {
    cd "$(dirname "${BASH_SOURCE[0]}")/../RL_Agents" 2> /dev/null && pwd
}

policy_start() {
    local log_dir="$1" port="$2" models="${3:-}" ungated="${4:-0}" dir
    dir="$(policy_dir)"
    if ! ls "${dir}"/artifacts/*/metadata.json > /dev/null 2>&1; then
        echo "==> no trained RL agent in RL_Agents/artifacts (python3 -m rl_agents.train): no RL bots" >&2
        return 1
    fi
    local log="${log_dir}/policy_server.log"
    # shellcheck disable=SC2086
    (cd "${dir}" && exec python3 -m rl_agents.serve --port "${port}" ${models:+--models ${models}} $([[ "${ungated}" -eq 1 ]] && echo --allow-ungated)) > "${log}" 2>&1 < /dev/null &
    POLICY_PID=$!
    for _ in $(seq 1 120); do
        grep -q "listening on port" "${log}" 2> /dev/null && break
        kill -0 "${POLICY_PID}" 2> /dev/null || break
        sleep 0.25
    done
    if ! grep -q "listening on port" "${log}" 2> /dev/null; then
        echo "==> the policy server did not start, see its log (last lines below)" >&2
        tail -5 "${log}" >&2
        policy_stop
        return 1
    fi
    POLICY_MODELS="$(awk -F: '/^  [A-Za-z0-9_]+: / { gsub(/ /, "", $1); printf "%s ", $1 }' "${log}")"
    echo "==> Policy server on port ${port}, agents: ${POLICY_MODELS}"
    return 0
}

# the seeds of the simulations the trained agents learned from (artifacts/*/metadata.json, provenance)
policy_training_seeds() {
    python3 - "$(policy_dir)/artifacts" << 'PY' 2> /dev/null
import json, sys
from pathlib import Path
seeds = set()
for meta in Path(sys.argv[1]).glob("*/metadata.json"):
    provenance = json.loads(meta.read_text()).get("provenance", {})
    for seed in [provenance.get("seed")] + list(provenance.get("seeds", [])):
        if seed is not None:
            seeds.add(str(seed))
print(" ".join(sorted(seeds)))
PY
}

policy_rl_models() {
    python3 - "$(policy_dir)/artifacts" << 'PY' 2> /dev/null
import json, sys
from pathlib import Path
meta = Path(sys.argv[1]) / "rl" / "metadata.json"
if meta.exists():
    m = json.loads(meta.read_text())
    names = [n for n in m.get("ensemble_members", []) if (meta.parent.parent / n / "metadata.json").exists()]
    print(" ".join(names or ["rl"]))
PY
}

policy_rl_gate() {
    python3 - "$(policy_dir)/artifacts" << 'PY' 2> /dev/null
import json, sys
from pathlib import Path
meta = Path(sys.argv[1]) / "rl" / "metadata.json"
print("passed" if meta.exists() and json.loads(meta.read_text()).get("gate_passed") else "failed")
PY
}

policy_stop() {
    if [[ -n "${POLICY_PID}" ]] && kill -0 "${POLICY_PID}" 2> /dev/null; then
        kill "${POLICY_PID}" 2> /dev/null || true
        wait "${POLICY_PID}" 2> /dev/null || true
    fi
    POLICY_PID=""
}
