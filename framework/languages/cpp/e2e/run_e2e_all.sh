#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCENARIO_TIMEOUT_SECONDS=1800

# Bind errors such as "already bound" are terminal for the selected config.
# The aggregate runner must expose that failure instead of classifying it as
# retryable, so a broken fixture cannot be reported as a later success.

POSITIONAL_ARGS=()
for arg in "$@"; do
  case "$arg" in
    --scenario-timeout-seconds=*) SCENARIO_TIMEOUT_SECONDS="${arg#*=}" ;;
    --*) echo "Unknown C++ E2E runner option '${arg}'." >&2; exit 2 ;;
    *) POSITIONAL_ARGS+=("$arg") ;;
  esac
done
set -- "${POSITIONAL_ARGS[@]}"
if [[ ! "$SCENARIO_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
  echo "--scenario-timeout-seconds must be a positive integer." >&2
  exit 2
fi

CONFIGS=(
  RegistrationCodec
  RegistryMessaging
  SpotService
  PubSub
  ResilienceLifecycle
  DiscoveryRegistryHa
  RuntimeMonitoring
  AutomaticTurnDispatch
  ToActorMessaging
  SpotActorTransfer
  ObservabilityOps
  ChannelEgressRouting
  SubmitAdmission
  InstanceSpot
)

START_ORDER_VARIANTS=(
  forward
  reverse
  shuffle:20260709
)

START_ORDER_CONFIGS=(
  RegistryMessaging
  SpotService
  ToActorMessaging
)

uses_start_order_axis() {
  local config="$1"
  local candidate
  for candidate in "${START_ORDER_CONFIGS[@]}"; do
    if [[ "$candidate" == "$config" ]]; then
      return 0
    fi
  done
  return 1
}

start_orders_for() {
  local config="$1"
  if uses_start_order_axis "$config"; then
    printf '%s\n' "${START_ORDER_VARIANTS[@]}"
  else
    printf '%s\n' forward
  fi
}

verify_start_order_contract() {
  local config
  for config in "${START_ORDER_CONFIGS[@]}"; do
    if ! rg -q -- '--start-order=' "${SCRIPT_DIR}/${config}/run_e2e.sh"; then
      echo "[cpp-e2e] start-order contract missing in ${config}" >&2
      return 1
    fi
  done
}

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
  echo "[cpp-e2e] interrupted; stopping the current configuration..." >&2
  exit 130
}

trap on_exit EXIT
trap on_interrupt INT TERM

SELECTED_CONFIGS=()
SELECTED_SCENARIOS=()

if [[ "$#" -eq 0 ]]; then
  "${SCRIPT_DIR}/verify_common_inventory.sh"
  for config in "${CONFIGS[@]}"; do
    SELECTED_CONFIGS+=("${config}")
    SELECTED_SCENARIOS+=("all")
  done
else
  for selector in "$@"; do
    config="${selector%%:*}"
    scenario="all"
    if [[ "$selector" == *:* ]]; then
      scenario="${selector#*:}"
      if [[ -z "$scenario" ]]; then
        echo "Missing scenario list in selector '${selector}'." >&2
        exit 2
      fi
    fi

    matched=0
    for known_config in "${CONFIGS[@]}"; do
      if [[ "$config" == "$known_config" ]]; then
        matched=1
        break
      fi
    done
    if [[ "$matched" == "0" ]]; then
      echo "Unknown e2e config selector '${config}'." >&2
      exit 2
    fi

    IFS=',' read -ra scenario_items <<<"${scenario}"
    for scenario_item in "${scenario_items[@]}"; do
      if [[ -z "${scenario_item}" ]]; then
        echo "Empty scenario in selector '${selector}'." >&2
        exit 2
      fi
      SELECTED_CONFIGS+=("${config}")
      SELECTED_SCENARIOS+=("${scenario_item}")
    done
  done
fi

run_config_once() {
  local config="$1"
  local scenario="$2"
  local start_order="$3"
  local output status started_at ended_at
  local -a runner_command=(./run_e2e.sh "${scenario}")
  if uses_start_order_axis "${config}"; then
    runner_command+=("--start-order=${start_order}")
  fi
  output="$(mktemp)"

  : >"${output}"
  started_at="$(date +%s)"
  set +e
  (
    cd "${SCRIPT_DIR}/${config}" &&
      exec timeout "${SCENARIO_TIMEOUT_SECONDS}s" "${runner_command[@]}"
  ) > >(tee "${output}") 2>&1 &
  active_config_pid="$!"
  wait "${active_config_pid}"
  status="$?"
  active_config_pid=""
  set -e
  ended_at="$(date +%s)"

  if [[ "${status}" == "0" ]]; then
    rm -f "${output}"
    echo "[cpp-e2e] ${config} PASS ($((ended_at - started_at))s, start_order=${start_order})"
    return 0
  fi

  echo "[cpp-e2e] ${config} FAIL ($((ended_at - started_at))s, start_order=${start_order})" >&2
  echo "[cpp-e2e] preserved output=${output}" >&2
  return "${status}"
}

all_started_at="$(date +%s)"
verify_start_order_contract
echo "[cpp-e2e] start configs=${#SELECTED_CONFIGS[@]} at=$(date -Is)"
for i in "${!SELECTED_CONFIGS[@]}"; do
  config="${SELECTED_CONFIGS[$i]}"
  scenario="${SELECTED_SCENARIOS[$i]}"
  mapfile -t selected_start_orders < <(start_orders_for "$config")
  for start_order in "${selected_start_orders[@]}"; do
    echo "[cpp-e2e] ${config} start scenario=${scenario} start_order=${start_order}"
    run_config_once "${config}" "${scenario}" "${start_order}"
  done
done
all_ended_at="$(date +%s)"

echo "[cpp-e2e] total PASS ($((all_ended_at - all_started_at))s)"
