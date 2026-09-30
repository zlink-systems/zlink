#!/usr/bin/env bash
# The common runner build step (launchers.py) for one role: perf spec §6.5, §17.5. The perf project consumes the
# published framework-cpp release package only: samples/bootstrap.cmake downloads and extracts it into .zlink/ and
# configures this project against it, exactly as a sample in package mode. Core and Framework are never built here.
# Usage: build_role.sh <cmake target, for example perf_session_server>
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [[ ! -f "${PERF_DIR}/build/CMakeCache.txt" ]]; then
  cmake -DZLINK_PROJECT_DIR="${PERF_DIR}" -P "${PERF_DIR}/../samples/bootstrap.cmake"
fi
cmake --build "${PERF_DIR}/build" --target "$1" --parallel "${ZLINK_PERF_BUILD_JOBS:-$(nproc)}"
