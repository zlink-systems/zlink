#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAX_ATTEMPTS="${ZLINK_E2E_RETRY_ATTEMPTS:-3}"
SCENARIO_TIMEOUT_SECONDS="${ZLINK_NODE_E2E_SCENARIO_TIMEOUT_SECONDS:-1800}"
DEFAULT_CONFIGS=(
  DiscoveryRegistryHa
  RegistrationCodec
  RegistryMessaging
  PubSub
  SpotService
  RuntimeMonitoring
  ResilienceLifecycle
  AutomaticTurnDispatch
  ObservabilityOps
  ToActorMessaging
  SpotActorTransfer
  SubmitAdmission
  ChannelEgressRouting
  InstanceSpot
)
BIND_RETRY_PATTERN="ZlinkBindException|BindException|Address already in use|EADDRINUSE|errno=98"

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
  echo "[node-e2e] interrupted; stopping the current Node.js configuration..." >&2
  exit 130
}

trap on_exit EXIT
trap on_interrupt INT TERM

selected_configs=()
selected_scenarios=()
RUN_START_ORDER_AXES=0

if [[ "$#" -eq 0 ]]; then
  RUN_START_ORDER_AXES=1
  for config in "${DEFAULT_CONFIGS[@]}"; do
    selected_configs+=("${config}")
    selected_scenarios+=("all")
  done
else
  for item in "$@"; do
    if [[ "${item}" == *:* ]]; then
      selected_configs+=("${item%%:*}")
      selected_scenarios+=("${item#*:}")
    else
      selected_configs+=("${item}")
      selected_scenarios+=("all")
    fi
  done
fi

for config in "${selected_configs[@]}"; do
  if [[ ! -x "${SCRIPT_DIR}/${config}/run_e2e.sh" ]]; then
    echo "[node-e2e] ${config} is missing an executable run_e2e.sh; aggregate cannot report PASS." >&2
    exit 2
  fi
done

run_config_with_retry() {
  local config="$1"
  local scenario="$2"
  local start_order="${3:-forward}"
  local attempt output status started_at ended_at
  output="$(mktemp)"

  local max_attempts="$MAX_ATTEMPTS"
  if [[ "$config" == "SubmitAdmission" ]]; then
    max_attempts=1
  fi

  for attempt in $(seq 1 "${max_attempts}"); do
    : >"${output}"
    started_at="$(date +%s)"
    set +e
    (
      cd "${SCRIPT_DIR}/${config}" &&
        exec env E2E_START_ORDER="${start_order}" nice -n 10 \
          timeout "${SCENARIO_TIMEOUT_SECONDS}s" ./run_e2e.sh "${scenario}"
    ) > >(tee "${output}") 2>&1 &
    active_config_pid="$!"
    wait "${active_config_pid}"
    status="$?"
    active_config_pid=""
    set -e
    ended_at="$(date +%s)"

    if [[ "${status}" == "0" ]]; then
      if ! grep -Eq 'result=passed|scenario [^[:space:]]+ passed|[[:space:]]PASS([[:space:]]|$)|"status":"PASS"' "${output}"; then
        echo "[node-e2e] ${config} exited 0 without a machine-visible PASS result." >&2
        rm -f "${output}"
        return 1
      fi
      rm -f "${output}"
      echo "[node-e2e] ${config} PASS order=${start_order} ($((ended_at - started_at))s)"
      return 0
    fi

    echo "[node-e2e] ${config} FAIL ($((ended_at - started_at))s, attempt ${attempt})" >&2
    if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
      rm -f "${output}"
      return "${status}"
    fi

    if [[ "${attempt}" == "${max_attempts}" ]]; then
      rm -f "${output}"
      return "${status}"
    fi

    echo "[node-e2e] ${config} retry after transient bind failure (${attempt}/${MAX_ATTEMPTS})" >&2
    sleep 1
  done
}

all_started_at="$(date +%s)"
echo "[node-e2e] start configs=${#selected_configs[@]} at=$(date -Is)"
for index in "${!selected_configs[@]}"; do
  config="${selected_configs[$index]}"
  scenario="${selected_scenarios[$index]}"
  echo "[node-e2e] ${config} start scenario=${scenario}"
  run_config_with_retry "${config}" "${scenario}"
done

if [[ "${RUN_START_ORDER_AXES}" == "1" ]]; then
  for config in RegistryMessaging SpotService ToActorMessaging; do
    run_config_with_retry "${config}" all reverse
    run_config_with_retry "${config}" all shuffle:20260715
  done
fi
all_ended_at="$(date +%s)"

echo "[node-e2e] total PASS ($((all_ended_at - all_started_at))s)"
