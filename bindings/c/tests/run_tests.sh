#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${C_DIR}/../.." && pwd)"
BUILD_DIR="${C_DIR}/build"
BUILD_JOBS="${ZLINK_BUILD_JOBS:-2}"

if grep -R -n -E 'core/include|ZLINK_CORE_INCLUDE_DIR|PERF_CORE_INCLUDE_DIR' \
  "${C_DIR}/CMakeLists.txt" \
  "${C_DIR}/samples/CMakeLists.txt" \
  "${C_DIR}/perf/CMakeLists.txt"; then
  echo "[FAIL] C binding samples/perf must include only bindings/c/include" >&2
  exit 1
fi

cmake -S "${C_DIR}" -B "${BUILD_DIR}" \
  -DZLINK_CORE_DIR="${ROOT_DIR}/core" \
  -DZLINK_C_CORE_BUILD_DIR="${ROOT_DIR}/core/build" \
  -DZLINK_C_BUILD_TESTS=ON \
  -DZLINK_C_BUILD_SAMPLES=OFF \
  -DZLINK_C_BUILD_BENCHMARKS=OFF \
  -DZLINK_C_BUILD_BENCHES=OFF \
  -DZLINK_BUILD_BENCH_ZMQ=OFF \
  -DZLINK_BUILD_BENCH_STREAMCOMPARE=OFF \
  -DZLINK_BUILD_BENCH_ROUTER_COMPARE=OFF

cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure -L contract

"${C_DIR}/samples/run_samples.sh"
