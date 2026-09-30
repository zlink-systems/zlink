#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../../scripts/java-env.sh"
exec flock --exclusive --close /tmp/zlink-samples-gate.lock \
  python3 "${SCRIPT_DIR}/../../../../../perf/runner/runner.py" single --language kotlin --perf-dir "${SCRIPT_DIR}/.." "$@"
