#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./bindings/cpp/build.sh [RUN_TESTS] [RUN_SAMPLES]
# Example:
#   ./bindings/cpp/build.sh ON ON

RUN_TESTS="${1:-ON}"
RUN_SAMPLES="${2:-ON}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CPP_DIR="${ROOT_DIR}/bindings/cpp"
CORE_BUILD_DIR="${ROOT_DIR}/core/build"
BUILD_DIR="${CPP_DIR}/build"

mkdir -p "${BUILD_DIR}"

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cached_src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" || true)"
  if [[ -n "${cached_src}" && "${cached_src}" != "${CPP_DIR}" ]]; then
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"
  fi
fi

cmake -S "${CPP_DIR}" -B "${BUILD_DIR}" \
  -DZLINK_CORE_DIR="${ROOT_DIR}/core" \
  -DZLINK_CPP_CORE_BUILD_DIR="${CORE_BUILD_DIR}" \
  -DZLINK_CPP_BUILD_TESTS="${RUN_TESTS}" \
  -DZLINK_CPP_BUILD_SAMPLES="${RUN_SAMPLES}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"
