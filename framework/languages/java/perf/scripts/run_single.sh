#!/usr/bin/env bash
set -euo pipefail
PERF_ENTRY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF_ENTRY_ROOT="$(cd "${PERF_ENTRY_DIR}/../../../../.." && pwd)"
exec python3 "${PERF_ENTRY_ROOT}/framework/perf-contract/runner.py" single --language java "$@"
