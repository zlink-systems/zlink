#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$(mktemp -d)"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
trap 'rm -rf "$CONFIG_DIR"' EXIT

# The delegated SpotService runner creates role files with write_role_config.py
# and owns the actual process readiness checks for this selector-only entry point.
scenario="${*:-all}"

case "$scenario" in
  IS-E2E-01|IS-E2E-02|IS-E2E-03|track-a)
    "$SCRIPT_DIR/../SpotService/run_e2e.sh" instance-track-a
    ;;
  IS-E2E-08|idle)
    "$SCRIPT_DIR/../SpotService/run_e2e.sh" instance-idle
    ;;
  *)
    cat >&2 <<EOF
InstanceSpot '${scenario}' is not executable yet.
The .NET process fixture currently covers IS-E2E-01 through IS-E2E-03 and IS-E2E-08.
The aggregate runner keeps Config 14 incomplete until the remaining scenarios
have their own process evidence.
EOF
    exit 2
    ;;
esac
