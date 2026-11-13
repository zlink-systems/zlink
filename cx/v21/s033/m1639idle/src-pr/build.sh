#!/usr/bin/env bash
# Builds the C++ with-grpc bench. Owner of the C++ build command; run_local.sh and build_all.sh call it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../../../.." && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
BUILD_DIR="${HERE}/build"

bench_require_low_load
# The C++ Framework resolves its dependencies through its vcpkg manifest, as the Framework's
# own configure does (scripts/gate/rebuild-dev.sh). gRPC comes from the same manifest through
# its "bench" feature, so one process links one protobuf.
[[ -n "${VCPKG_ROOT:-}" && -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]] || {
  echo "VCPKG_ROOT must point to a bootstrapped vcpkg tree; setup: scripts/gate/check-env.sh" >&2
  exit 1
}
export VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY:-4}"

CMAKE_ARGS=(
  -S "${HERE}" -B "${BUILD_DIR}" -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  -DVCPKG_MANIFEST_DIR="${REPO_ROOT}/framework/languages/cpp"
  -DVCPKG_MANIFEST_FEATURES=bench
  -DVCPKG_OVERLAY_PORTS="${REPO_ROOT}/vcpkg/ports"
)

CMAKE_CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"
if [[ -f "${CMAKE_CACHE_FILE}" ]]; then
  LOCAL_PACKAGE_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-${REPO_ROOT}/.artifacts/wsl}"
  BINDING_VERSION="$(sed -n 's/^ZLINK_BINDING_VERSION=//p' "${REPO_ROOT}/bindings/cpp/VERSION")"
  CORE_VERSION="$(sed -n 's/^LIBZLINK_VERSION=//p' "${REPO_ROOT}/VERSION")"
  BINDING_PREFIX="${LOCAL_PACKAGE_ROOT}/install/zlink-cpp/${BINDING_VERSION}"
  CORE_PREFIX="${LOCAL_PACKAGE_ROOT}/install/zlink-core/${CORE_VERSION}"

  cache_has_value() {
    grep -Fqx -- "$1:$2=$3" "${CMAKE_CACHE_FILE}"
  }
  cache_path_is_beneath() {
    local cached_path
    cached_path="$(sed -n "s/^$1:[^=]*=//p" "${CMAKE_CACHE_FILE}")"
    [[ "${cached_path}" == "${2%/}/"* ]]
  }

  # CMake preserves CACHE values across configures. If the requested local package
  # input changed, discard both prefix inputs and find_package's resolved config dirs
  # before the configure so all targets resolve the requested binding and Core.
  if ! cache_has_value ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION STRING "${BINDING_VERSION}" \
      || ! cache_has_value ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION STRING "${CORE_VERSION}" \
      || ! cache_has_value ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX PATH "${BINDING_PREFIX}" \
      || ! cache_has_value ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX PATH "${CORE_PREFIX}" \
      || ! cache_path_is_beneath zlink_cpp_DIR "${BINDING_PREFIX}" \
      || ! cache_path_is_beneath zlink_DIR "${CORE_PREFIX}"; then
    CMAKE_ARGS+=(
      -U ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT
      -U ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION
      -U ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION
      -U ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX
      -U ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX
      -U zlink_cpp_DIR
      -U zlink_DIR
    )
  fi
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --parallel 2
