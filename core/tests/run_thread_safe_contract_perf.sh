#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/core/build"
MIN_RATIO="${ZLINK_PERF_MIN_RATIO:-0.80}"

TEST_NAMES=(
  test_thread_safe_scaling_raw
)

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Run thread-safe scaling contract cases via CTest.

Options:
  --build-dir PATH   Build directory containing CTest metadata
                     (default: ${BUILD_DIR})
  --min-ratio VALUE  Minimum allowed 64-handle per-handle throughput ratio
                     (default: ${MIN_RATIO})
  -h, --help         Show this help text

Examples:
  $(basename "$0")
  $(basename "$0") --min-ratio 0.85
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --min-ratio)
      MIN_RATIO="$2"
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

for test_name in "${TEST_NAMES[@]}"; do
  echo "=== Running ${test_name} (min ratio ${MIN_RATIO}) ==="
  ZLINK_PERF_MIN_RATIO="${MIN_RATIO}" \
    ctest --test-dir "${BUILD_DIR}" \
      --output-on-failure \
      -R "^${test_name}$"
done

echo "=== Thread-safe scaling perf contract completed ==="
