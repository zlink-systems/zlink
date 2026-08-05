#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
NORMALIZE_TIMESTAMPS_SH="${SCRIPT_DIR}/tools/normalize_build_timestamps.sh"
export TMPDIR="${TMPDIR:-/tmp}"
BUILD_JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
MAKE_BIN="$(command -v gmake || command -v make)"

echo "=== Clean Build ==="
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
echo "=== CMake Configure ==="
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON -DWITH_DOCS=OFF -DWITH_TLS=ON -DBUILD_BENCHMARKS=ON \
  -DCMAKE_MAKE_PROGRAM="${MAKE_BIN}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

if [ -f "${BUILD_DIR}/compile_commands.json" ]; then
  ln -sfn "${BUILD_DIR}/compile_commands.json" "${ROOT_DIR}/compile_commands.json"
  echo "Linked compile_commands.json -> ${BUILD_DIR}/compile_commands.json"
fi

echo "=== Build ==="
bash "${NORMALIZE_TIMESTAMPS_SH}" "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"

echo "=== Run Tests ==="
bash "${SCRIPT_DIR}/tests/run_test_lanes.sh" \
  --build-dir "${BUILD_DIR}" \
  --include-e2e

echo "=== Done ==="
