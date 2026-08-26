#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DELIVERYDISPATCH_WAIT_INTERVAL_MS=100
export DELIVERYDISPATCH_WAIT_ATTEMPTS=300
node "${SCRIPT_DIR}/../run-sample.mjs" "${SCRIPT_DIR}/Runner/sample-runner.mjs"
