#!/usr/bin/env bash
set -euo pipefail

# Aggregate e2e runner template.
# It only stops the current config process and delegates Redis cleanup to it.
# Expected layout:
#   e2e/redis-common.sh
#   e2e/run_e2e_all.sh
#   e2e/<Config>/run_e2e.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANGUAGE="${ZLINK_E2E_LANGUAGE:-java}"
MAX_ATTEMPTS="${ZLINK_E2E_RETRY_ATTEMPTS:-3}"
SCENARIO_TIMEOUT_SECONDS="${ZLINK_E2E_SCENARIO_TIMEOUT_SECONDS:-1800}"
BIND_RETRY_PATTERN="Address already in use|EADDRINUSE|already bound|errno=98"

CONFIGS=(
  ExampleConfigA
  ExampleConfigB
)

cleanup_done=0
active_config_pid=""

cleanup_resources() {
  if [[ "${cleanup_done}" == "1" ]]; then
    return
  fi
  cleanup_done=1

  if [[ -n "${active_config_pid}" ]] && kill -0 "${active_config_pid}" >/dev/null 2>&1; then
    kill -TERM "${active_config_pid}" >/dev/null 2>&1 || true
    wait "${active_config_pid}" >/dev/null 2>&1 || true
  fi
}

on_exit() {
  local code=$?
  cleanup_resources
  exit "${code}"
}

on_interrupt() {
  echo "[${LANGUAGE}-e2e] interrupted; stopping the current configuration..." >&2
  exit 130
}

trap on_exit EXIT
trap on_interrupt INT TERM

SELECTED_CONFIGS=()
SELECTED_SCENARIOS=()

if [[ "$#" -eq 0 ]]; then
  for config in "${CONFIGS[@]}"; do
    SELECTED_CONFIGS+=("${config}")
    SELECTED_SCENARIOS+=("all")
  done
else
  for selector in "$@"; do
    config="${selector%%:*}"
    scenario="all"
    if [[ "${selector}" == *:* ]]; then
      scenario="${selector#*:}"
      if [[ -z "${scenario}" ]]; then
        echo "Missing scenario list in selector '${selector}'." >&2
        exit 2
      fi
    fi
    SELECTED_CONFIGS+=("${config}")
    SELECTED_SCENARIOS+=("${scenario}")
  done
fi

run_config_with_retry() {
  local config="$1"
  local scenario="$2"
  local attempt output status started_at ended_at
  output="$(mktemp)"

  for attempt in $(seq 1 "${MAX_ATTEMPTS}"); do
    : >"${output}"
    started_at="$(date +%s)"
    set +e
    (
      cd "${SCRIPT_DIR}/${config}" &&
        exec nice -n 10 timeout "${SCENARIO_TIMEOUT_SECONDS}s" ./run_e2e.sh "${scenario}"
    ) > >(tee "${output}") 2>&1 &
    active_config_pid="$!"
    wait "${active_config_pid}"
    status="$?"
    active_config_pid=""
    set -e
    ended_at="$(date +%s)"

    if [[ "${status}" == "0" ]]; then
      rm -f "${output}"
      echo "[${LANGUAGE}-e2e] ${config} PASS ($((ended_at - started_at))s)"
      return 0
    fi

    echo "[${LANGUAGE}-e2e] ${config} FAIL ($((ended_at - started_at))s, attempt ${attempt})" >&2
    if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
      rm -f "${output}"
      return "${status}"
    fi

    if [[ "${attempt}" == "${MAX_ATTEMPTS}" ]]; then
      rm -f "${output}"
      return "${status}"
    fi

    echo "[${LANGUAGE}-e2e] ${config} retry after transient bind failure (${attempt}/${MAX_ATTEMPTS})" >&2
    sleep 1
  done
}

all_started_at="$(date +%s)"
echo "[${LANGUAGE}-e2e] start configs=${#SELECTED_CONFIGS[@]} at=$(date -Is)"
for i in "${!SELECTED_CONFIGS[@]}"; do
  config="${SELECTED_CONFIGS[$i]}"
  scenario="${SELECTED_SCENARIOS[$i]}"
  echo "[${LANGUAGE}-e2e] ${config} start scenario=${scenario}"
  run_config_with_retry "${config}" "${scenario}"
done
all_ended_at="$(date +%s)"

echo "[${LANGUAGE}-e2e] total PASS ($((all_ended_at - all_started_at))s)"
