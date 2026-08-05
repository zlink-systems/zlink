#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node "${SCRIPT_DIR}/../run-sample.mjs" "${SCRIPT_DIR}/Runner/sample-runner.mjs"
