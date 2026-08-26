#!/usr/bin/env bash
set -euo pipefail

export SHOPPINGMALL_WAIT_ATTEMPTS=300

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node "${SCRIPT_DIR}/../run-sample.mjs" "${SCRIPT_DIR}/Runner/sample-runner.mjs"
