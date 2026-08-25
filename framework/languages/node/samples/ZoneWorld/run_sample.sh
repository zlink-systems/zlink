#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANE_ARGS=()
if [[ "${1:-}" == "ZW-B8" || "${1:-}" == "--b8-child" ]]; then
  LANE_ARGS=(--lane b8)
  shift
fi
node "${SCRIPT_DIR}/../run-sample.mjs" "${SCRIPT_DIR}/Runner/sample-runner.mjs" "${LANE_ARGS[@]+"${LANE_ARGS[@]}"}" "$@"
