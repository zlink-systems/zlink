#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/core/build"
LANES="unittest,integration"

resolve_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
    return
  fi
  if command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN
    return
  fi
  echo 1
}

UNITTEST_JOBS="$(resolve_jobs)"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Run test lanes sequentially from a configured build directory.

Options:
  --build-dir PATH       Build directory containing CTest metadata
                         (default: ${BUILD_DIR})
  --lanes LIST           Comma-separated lane list. Supported lanes:
                         unittest,integration,e2e,regression
                         (default: ${LANES})
  --include-e2e          Append e2e to the default lane list
  --include-regression   Append regression to the selected lane list
  --unittest-jobs N      Parallel jobs for the unittest lane
                         (default: ${UNITTEST_JOBS})
  -h, --help             Show this help text

Examples:
  $(basename "$0")
  $(basename "$0") --include-e2e
  $(basename "$0") --include-e2e --include-regression
  $(basename "$0") --lanes unittest,integration
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --lanes)
      LANES="$2"
      shift 2
      ;;
    --include-e2e)
      if [[ ",${LANES}," != *",e2e,"* ]]; then
        LANES="${LANES},e2e"
      fi
      shift
      ;;
    --include-regression)
      if [[ ",${LANES}," != *",regression,"* ]]; then
        LANES="${LANES},regression"
      fi
      shift
      ;;
    --unittest-jobs)
      UNITTEST_JOBS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
  echo "Build directory does not look configured for CTest: ${BUILD_DIR}" >&2
  exit 1
fi

run_lane() {
  local lane="$1"
  local jobs="$2"

  echo "=== Running ${lane} lane (jobs=${jobs}) ==="
  ctest --test-dir "${BUILD_DIR}" \
    --output-on-failure \
    -L "^${lane}$" \
    -j "${jobs}"
}

IFS=',' read -r -a LANE_ARRAY <<< "${LANES}"
for lane in "${LANE_ARRAY[@]}"; do
  case "${lane}" in
    unittest)
      run_lane "${lane}" "${UNITTEST_JOBS}"
      ;;
    integration|e2e|regression)
      run_lane "${lane}" 1
      ;;
    "")
      ;;
    *)
      echo "Unsupported lane: ${lane}" >&2
      exit 1
      ;;
  esac
done

echo "=== Test lanes completed ==="
