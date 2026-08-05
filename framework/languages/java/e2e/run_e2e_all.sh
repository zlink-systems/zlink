#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAX_ATTEMPTS=3
SCENARIO_TIMEOUT_SECONDS=1800
BIND_RETRY_PATTERN="ZlinkBindException|BindException|Address already in use|EADDRINUSE|errno=98"

DEFAULT_SCENARIOS=(
  StoreFailure
  RegistrationCodec
  RegistryMessaging
  PubSub
  SpotService
  RuntimeMonitoring
  ResilienceLifecycle
  AutomaticTurnDispatch
  ToActorMessaging
  SpotActorTransfer
  ObservabilityOps
  ChannelEgressRouting
  SubmitAdmission
  InstanceSpot
)
START_ORDER_CONFIGS=(RegistryMessaging SpotService ToActorMessaging)
START_ORDER_MODES=(reverse "shuffle:20260715")

cleanup_done=0
active_scenario_pid=""

cleanup_resources() {
  if [[ "${cleanup_done}" == "1" ]]; then
    return
  fi
  cleanup_done=1

  if [[ -n "${active_scenario_pid}" ]] && kill -0 "${active_scenario_pid}" >/dev/null 2>&1; then
    kill -TERM "${active_scenario_pid}" >/dev/null 2>&1 || true
    wait "${active_scenario_pid}" >/dev/null 2>&1 || true
  fi
}

on_exit() {
  local code=$?
  cleanup_resources
  exit "${code}"
}

on_interrupt() {
  echo "[java-e2e] interrupted; stopping the current configuration..." >&2
  exit 130
}

trap on_exit EXIT
trap on_interrupt INT TERM

selected_scenarios=()
selected_selectors=()

if [[ "$#" -eq 0 ]]; then
  for scenario in "${DEFAULT_SCENARIOS[@]}"; do
    selected_scenarios+=("${scenario}")
    selected_selectors+=("all")
  done
else
  for selector in "$@"; do
    if [[ "${selector}" == *:* ]]; then
      selected_scenarios+=("${selector%%:*}")
      selected_selectors+=("${selector#*:}")
    else
      selected_scenarios+=("${selector}")
      selected_selectors+=("all")
    fi
  done
fi

validate_selected_suites() {
  local index scenario suite_dir runner
  for index in "${!selected_scenarios[@]}"; do
    scenario="${selected_scenarios[$index]}"
    suite_dir="$SCRIPT_DIR/$scenario"
    runner="$suite_dir/run_e2e.sh"
    if [[ ! -d "$suite_dir" ]]; then
      echo "[java-e2e] aggregate_incomplete reason=missing_suite suite=$scenario" >&2
      return 1
    fi
    if [[ ! -x "$runner" ]]; then
      echo "[java-e2e] aggregate_incomplete reason=missing_runner suite=$scenario runner=$runner" >&2
      return 1
    fi
  done
}

validate_selected_suites

run_scenario_with_retry() {
  local scenario="$1"
  local selector="$2"
  local start_order="${3:-forward}"
  local attempt output status started_at ended_at
  output="$(mktemp)"

  for attempt in $(seq 1 "${MAX_ATTEMPTS}"); do
    : >"${output}"
    started_at="$(date +%s)"
    set +e
    (
      cd "$SCRIPT_DIR/$scenario" &&
        exec nice -n 10 timeout "${SCENARIO_TIMEOUT_SECONDS}s" \
          ./run_e2e.sh "${selector}" --start-order "${start_order}"
    ) > >(tee "${output}") 2>&1 &
    active_scenario_pid="$!"
    wait "${active_scenario_pid}"
    status="$?"
    active_scenario_pid=""
    set -e
    ended_at="$(date +%s)"

    if [[ "${status}" == "0" ]]; then
      rm -f "${output}"
      echo "[java-e2e] ${scenario} PASS start_order=${start_order} ($((ended_at - started_at))s)"
      return 0
    fi

    echo "[java-e2e] ${scenario} FAIL start_order=${start_order} ($((ended_at - started_at))s, attempt ${attempt})" >&2
    if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
      rm -f "${output}"
      return "${status}"
    fi

    if [[ "${attempt}" == "${MAX_ATTEMPTS}" ]]; then
      rm -f "${output}"
      return "${status}"
    fi

    echo "[java-e2e] ${scenario} retry after transient bind failure (${attempt}/${MAX_ATTEMPTS})" >&2
    sleep 1
  done
}

all_started_at="$(date +%s)"
echo "[java-e2e] start configs=${#selected_scenarios[@]} at=$(date -Is)"
for index in "${!selected_scenarios[@]}"; do
  scenario="${selected_scenarios[$index]}"
  selector="${selected_selectors[$index]}"
  echo "[java-e2e] ${scenario} start scenario=${selector}"
  run_scenario_with_retry "${scenario}" "${selector}" forward
done

if [[ "$#" -eq 0 ]]; then
  for start_order in "${START_ORDER_MODES[@]}"; do
    for scenario in "${START_ORDER_CONFIGS[@]}"; do
      echo "[java-e2e] ${scenario} start scenario=all start_order=${start_order}"
      run_scenario_with_retry "${scenario}" all "${start_order}"
    done
  done
fi
all_ended_at="$(date +%s)"

echo "[java-e2e] total PASS ($((all_ended_at - all_started_at))s)"
