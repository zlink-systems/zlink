#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec flock --exclusive --close /tmp/zlink-samples-gate.lock \
  python3 "${SCRIPT_DIR}/../../../../perf/runner/runner.py" single --language cpp --perf-dir "${SCRIPT_DIR}/.." "$@"
