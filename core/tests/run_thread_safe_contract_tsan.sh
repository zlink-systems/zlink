#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/core/build-tsan"
GENERATOR_ARGS=()

TEST_NAMES=(
  test_monitor_enhanced
  test_monitor_perf_contract
)

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Configure a TSAN build and run the thread-safe contract regression lane.

Options:
  --build-dir PATH   Build directory to use
                     (default: ${BUILD_DIR})
  --generator NAME   CMake generator to pass through
  -h, --help         Show this help text

Examples:
  $(basename "$0")
  $(basename "$0") --build-dir ${ROOT_DIR}/core/build-tsan-clang
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --generator)
      GENERATOR_ARGS=(-G "$2")
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

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  "${GENERATOR_ARGS[@]}" \
  -DBUILD_TESTS=ON \
  -DBUILD_STATIC=ON \
  -DBUILD_SHARED=ON \
  -DENABLE_TSAN=ON

cmake --build "${BUILD_DIR}" -j"$(nproc)" \
  --target test_monitor_enhanced test_monitor_perf_contract

for test_name in "${TEST_NAMES[@]}"; do
  echo "=== TSAN ${test_name} ==="
  ctest --test-dir "${BUILD_DIR}" \
    --output-on-failure \
    -R "^${test_name}$"
done

echo "=== Thread-safe contract TSAN lane completed ==="
