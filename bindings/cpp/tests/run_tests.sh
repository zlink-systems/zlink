#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${CPP_DIR}/../.." && pwd)"
export ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-local}"
source "${ROOT_DIR}/bindings/tools/local_core_runtime.sh"
if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
  CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX}"
else
  CORE_BUILD_DIR="${ROOT_DIR}/core/build"
fi
BUILD_DIR="${CPP_DIR}/build"

CONFIGURE_ARGS=(
  -DZLINK_CORE_DIR=${ZLINK_CORE_PACKAGE_PREFIX:-${ROOT_DIR}/core}
  -DZLINK_CPP_CORE_BUILD_DIR=${CORE_BUILD_DIR}
  -DZLINK_CPP_CORE_PACKAGE_PREFIX=
  -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON
  -DZLINK_CPP_BUILD_TESTS=ON
  -DZLINK_CPP_BUILD_SAMPLES=OFF
  -DZLINK_CPP_BUILD_BENCHMARKS=OFF
)

if [[ $# -gt 0 ]]; then
  CONFIGURE_ARGS+=("$@")
fi

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]] && \
    ! grep -q '^ZLINK_CPP_BUILD_TESTS:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt"; then
  echo "[cpp-tests] reset stale non-test build: ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

echo "[cpp-tests] configure: ${BUILD_DIR}"
cmake -S "${CPP_DIR}" -B "${BUILD_DIR}" "${CONFIGURE_ARGS[@]}"

echo "[cpp-tests] build"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "[cpp-tests] run contract tests"
if ctest --test-dir "${BUILD_DIR}" --output-on-failure -L contract && \
    "${CPP_DIR}/samples/run_samples.sh"; then
  echo "[cpp-tests] PASS"
else
  echo "[cpp-tests] FAIL"
  exit 1
fi
