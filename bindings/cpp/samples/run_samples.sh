#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${CPP_DIR}/../.." && pwd)"
CORE_BUILD_DIR="${ROOT_DIR}/core/build"
BUILD_DIR="${CPP_DIR}/build"

CONFIGURE_ARGS=(
  -DZLINK_CORE_DIR=${ROOT_DIR}/core
  -DZLINK_CPP_CORE_BUILD_DIR=${CORE_BUILD_DIR}
  -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON
  -DZLINK_CPP_BUILD_SAMPLES=ON
  -DZLINK_CPP_BUILD_TESTS=OFF
  -DZLINK_CPP_BUILD_BENCHMARKS=OFF
)

if [[ $# -gt 0 ]]; then
  CONFIGURE_ARGS+=("$@")
fi

echo "[cpp-samples] configure: ${BUILD_DIR}"
cmake -S "${CPP_DIR}" -B "${BUILD_DIR}" "${CONFIGURE_ARGS[@]}"

echo "[cpp-samples] build"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "[cpp-samples] run sample smoke tests"
ctest --test-dir "${BUILD_DIR}" --output-on-failure -L sample-smoke
