#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${C_DIR}/../.." && pwd)"
CORE_BUILD_DIR="${ROOT_DIR}/core/build"
BUILD_DIR="${C_DIR}/build"
BUILD_JOBS="${ZLINK_BUILD_JOBS:-2}"

CONFIGURE_ARGS=(
  -DZLINK_CORE_DIR=${ROOT_DIR}/core
  -DZLINK_C_CORE_BUILD_DIR=${CORE_BUILD_DIR}
  -DZLINK_C_BUILD_SAMPLES=ON
  -DZLINK_C_BUILD_TESTS=OFF
  -DZLINK_C_BUILD_BENCHMARKS=OFF
  -DZLINK_C_BUILD_BENCHES=OFF
  -DZLINK_BUILD_BENCH_ZMQ=OFF
  -DZLINK_BUILD_BENCH_STREAMCOMPARE=OFF
  -DZLINK_BUILD_BENCH_ROUTER_COMPARE=OFF
)

if [[ $# -gt 0 ]]; then
  CONFIGURE_ARGS+=("$@")
fi

echo "[c-samples] configure: ${BUILD_DIR}"
cmake -S "${C_DIR}" -B "${BUILD_DIR}" "${CONFIGURE_ARGS[@]}"

echo "[c-samples] build"
cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"

echo "[c-samples] run"
ctest --test-dir "${BUILD_DIR}" --output-on-failure -L sample-smoke
