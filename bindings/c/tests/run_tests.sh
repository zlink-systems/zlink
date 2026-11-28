#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${C_DIR}/../.." && pwd)"
source "${ROOT_DIR}/bindings/tools/local_core_runtime.sh"
BUILD_DIR="${C_DIR}/build-tests"
BUILD_JOBS="${ZLINK_BUILD_JOBS:-2}"
CORE_BUILD_DIR="${ZLINK_C_CORE_BUILD_DIR:-${ZLINK_CORE_PACKAGE_PREFIX:-${ROOT_DIR}/core/build}}"

if grep -R -n -E 'core/include|ZLINK_CORE_INCLUDE_DIR|PERF_CORE_INCLUDE_DIR' \
  "${C_DIR}/CMakeLists.txt" \
  "${C_DIR}/samples/CMakeLists.txt" \
  "${C_DIR}/perf/CMakeLists.txt"; then
  echo "[FAIL] C binding samples/perf must include only bindings/c/include" >&2
  exit 1
fi

cmake -S "${C_DIR}" -B "${BUILD_DIR}" \
  -DZLINK_CORE_DIR="${ZLINK_CORE_PACKAGE_PREFIX:-${ROOT_DIR}/core}" \
  -DZLINK_C_CORE_BUILD_DIR="${CORE_BUILD_DIR}" \
  -DZLINK_C_BUILD_TESTS=ON \
  -DZLINK_C_BUILD_SAMPLES=OFF \
  -DZLINK_C_BUILD_BENCHMARKS=ON \
  -DZLINK_C_BUILD_BENCHES=ON \
  -DZLINK_C_BUILD_NATIVE_BENCHES_ONLY=ON \
  -DZLINK_C_BUILD_BENCH_ZMQ=ON \
  -DZLINK_C_BUILD_BENCH_STREAMCOMPARE=ON \
  -DZLINK_C_BUILD_BENCH_ROUTER_COMPARE=ON \
  -DZLINK_BUILD_WITH_ROUTING_ZLINK_BENCHES=ON \
  -DZLINK_BUILD_BENCH_ZMQ=OFF \
  -DZLINK_BUILD_BENCH_STREAMCOMPARE=OFF \
  -DZLINK_BUILD_BENCH_ROUTER_COMPARE=OFF

cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure -L contract
ctest --test-dir "${BUILD_DIR}" --output-on-failure --no-tests=error \
  -L '^perf-(metric|regression)$'

"${C_DIR}/samples/run_samples.sh"
