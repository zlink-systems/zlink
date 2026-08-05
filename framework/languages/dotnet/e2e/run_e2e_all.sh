#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_TIMEOUT_SECONDS=1800

CONFIGS=(
  LocationMessaging
  PubSub
  RegistrationCodec
  ResilienceLifecycle
  RuntimeMonitoring
  SpotService
  SpotActorTransfer
  StoreFailure
  ToActorMessaging
  AutomaticTurnDispatch
  ObservabilityOps
  SubmitAdmission
  ChannelEgressRouting
  InstanceSpot
)

cleanup_done=0
active_config_pid=""

cleanup_resources() {
  if [[ "$cleanup_done" == "1" ]]; then
    return
  fi
  cleanup_done=1

  if [[ -n "$active_config_pid" ]] && kill -0 "$active_config_pid" >/dev/null 2>&1; then
    kill -TERM "$active_config_pid" >/dev/null 2>&1 || true
    wait "$active_config_pid" >/dev/null 2>&1 || true
  fi
}

on_exit() {
  local code=$?
  cleanup_resources
  exit "$code"
}

on_interrupt() {
  echo "[dotnet-e2e] interrupted; stopping the current configuration..." >&2
  exit 130
}

trap on_exit EXIT
trap on_interrupt INT TERM

SELECTED_CONFIGS=()
SELECTED_SCENARIOS=()
SELECTORS=()

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --config-timeout-seconds)
      CONFIG_TIMEOUT_SECONDS="$2"
      shift 2
      ;;
    --)
      shift
      SELECTORS+=("$@")
      break
      ;;
    *)
      SELECTORS+=("$1")
      shift
      ;;
  esac
done

if [[ "${#SELECTORS[@]}" -eq 0 ]]; then
  for config in "${CONFIGS[@]}"; do
    SELECTED_CONFIGS+=("$config")
    SELECTED_SCENARIOS+=("all")
  done
else
  for selector in "${SELECTORS[@]}"; do
    config="${selector%%:*}"
    scenario="all"
    if [[ "$selector" == *:* ]]; then
      scenario="${selector#*:}"
      if [[ -z "$scenario" ]]; then
        echo "Missing scenario list in selector '$selector'." >&2
        exit 2
      fi
    fi

    known=0
    for candidate in "${CONFIGS[@]}"; do
      if [[ "$candidate" == "$config" ]]; then
        known=1
        break
      fi
    done
    if [[ "$known" == "0" ]]; then
      echo "Unknown e2e config '$config'." >&2
      exit 2
    fi

    SELECTED_CONFIGS+=("$config")
    SELECTED_SCENARIOS+=("$scenario")
  done
fi

run_config() {
  local config="$1"
  local scenario="$2"
  local status started_at ended_at
  started_at="$(date +%s)"
  set +e
  (
    cd "$SCRIPT_DIR/$config" &&
      exec nice -n 10 timeout "${CONFIG_TIMEOUT_SECONDS}s" ./run_e2e.sh "$scenario"
  ) &
  active_config_pid="$!"
  wait "$active_config_pid"
  status="$?"
  active_config_pid=""
  set -e
  ended_at="$(date +%s)"

  if [[ "$status" != "0" ]]; then
    echo "[dotnet-e2e] ${config} FAIL ($((ended_at - started_at))s)" >&2
    return "$status"
  fi

  echo "[dotnet-e2e] ${config} PASS ($((ended_at - started_at))s)"
}

all_started_at="$(date +%s)"
echo "[dotnet-e2e] start configs=${#SELECTED_CONFIGS[@]} at=$(date -Is)"
for i in "${!SELECTED_CONFIGS[@]}"; do
  config="${SELECTED_CONFIGS[$i]}"
  scenario="${SELECTED_SCENARIOS[$i]}"
  echo "[dotnet-e2e] ${config} start scenario=${scenario}"
  run_config "$config" "$scenario"
done
all_ended_at="$(date +%s)"

echo "[dotnet-e2e] total PASS ($((all_ended_at - all_started_at))s)"
