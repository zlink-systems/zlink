#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
zlink_dotnet_e2e_acquire_run_lock "$0" "$@"
CONFIG_DIR="$(mktemp -d)"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
trap 'rm -rf "$CONFIG_DIR"' EXIT

# The delegated SpotService runner creates role files with write_role_config.py
# and owns the actual process readiness checks for this selector-only entry point.
scenario="${*:-all}"

case "$scenario" in
  IS-E2E-01|IS-E2E-02|IS-E2E-03|track-a)
    bash "$SCRIPT_DIR/../SpotService/run_e2e.sh" instance-track-a
    ;;
  IS-E2E-08|idle)
    bash "$SCRIPT_DIR/../SpotService/run_e2e.sh" instance-idle
    ;;
  IS-E2E-05|owner-loss)
    bash "$SCRIPT_DIR/../SpotService/run_e2e.sh" instance-owner-loss
    ;;
  IS-E2E-35|queue-owner-loss)
    bash "$SCRIPT_DIR/../SpotService/run_e2e.sh" instance-queue-owner-loss
    ;;
  creating-positive)
    bash "$SCRIPT_DIR/../SpotService/run_e2e.sh" instance-creating-join
    ;;
  *)
    cat >&2 <<EOF
InstanceSpot '${scenario}' is not executable yet.
The .NET process fixture currently covers IS-E2E-01 through IS-E2E-03, IS-E2E-05,
IS-E2E-08 and IS-E2E-35.
The aggregate runner keeps Config 14 incomplete until the remaining scenarios
have their own process evidence.
EOF
    exit 2
    ;;
esac
