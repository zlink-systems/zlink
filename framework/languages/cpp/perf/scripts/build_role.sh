#!/usr/bin/env bash
# Build one C++ perf role against the package source selected by the common runner.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(git -C "${PERF_DIR}" rev-parse --show-toplevel)"

if [[ $# -ne 1 ]]; then
  echo "Usage: build_role.sh <cmake target>" >&2
  exit 2
fi
if [[ -z "${ZLINK_PERF_FRAMEWORK_VERSION:-}" ]]; then
  echo "ZLINK_PERF_FRAMEWORK_VERSION is required; run the build through the common perf runner." >&2
  exit 2
fi
if [[ -z "${ZLINK_PERF_PACKAGE_SOURCE:-}" ]]; then
  echo "ZLINK_PERF_PACKAGE_SOURCE is required; run the build through the common perf runner." >&2
  exit 2
fi
if [[ ! "${ZLINK_PERF_FRAMEWORK_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Invalid ZLINK_PERF_FRAMEWORK_VERSION: ${ZLINK_PERF_FRAMEWORK_VERSION}" >&2
  exit 2
fi
if [[ "${ZLINK_PERF_PACKAGE_SOURCE}" != published && "${ZLINK_PERF_PACKAGE_SOURCE}" != local ]]; then
  echo "Unsupported ZLINK_PERF_PACKAGE_SOURCE: ${ZLINK_PERF_PACKAGE_SOURCE}" >&2
  exit 2
fi

if [[ "${ZLINK_PERF_PACKAGE_SOURCE}" == local ]]; then
  framework_source="${REPO_ROOT}/framework/languages/cpp"
  source "${framework_source}/samples/sample-build-common.sh"
  export ZLINK_CPP_BUILD_DIR="${ZLINK_CPP_BUILD_DIR:-${PERF_DIR}/.zlink/local-framework-build}"
  zlink_cpp_sample_prepare_repository_build "${framework_source}"
  install_prefix="${PERF_DIR}/.zlink/install"
  cmake -E remove_directory "${install_prefix}"
  cmake -E remove_directory "${PERF_DIR}/.zlink/downloads"
  cmake -S "${framework_source}" -B "${BUILD_DIR}" \
    "-DCMAKE_INSTALL_PREFIX=${install_prefix}" \
    -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=ON \
    -DZLINK_FRAMEWORK_CPP_STAGE_STANDALONE_DEPENDENCIES=ON \
    -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
    -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=OFF \
    -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
    -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
  cmake --build "${BUILD_DIR}" --target install --parallel "${ZLINK_PERF_BUILD_JOBS:-$(nproc)}"

  perf_prefix_path="${install_prefix}"
  framework_cache="${BUILD_DIR}/CMakeCache.txt"
  if [[ -f "${framework_cache}" ]]; then
    vcpkg_installed_dir="$(sed -n 's/^VCPKG_INSTALLED_DIR:[^=]*=//p' "${framework_cache}" | head -n 1)"
    vcpkg_target_triplet="$(sed -n 's/^VCPKG_TARGET_TRIPLET:[^=]*=//p' "${framework_cache}" | head -n 1)"
    vcpkg_dependency_prefix="${vcpkg_installed_dir}/${vcpkg_target_triplet}"
    if [[ -n "${vcpkg_installed_dir}" && -n "${vcpkg_target_triplet}" && -d "${vcpkg_dependency_prefix}" ]]; then
      perf_prefix_path="${perf_prefix_path};${vcpkg_dependency_prefix}"
    fi
  fi
  # The same generator rule as samples/bootstrap.cmake, so the published and local modes share build/.
  generator=(-G "Unix Makefiles")
  if command -v ninja > /dev/null; then
    generator=(-G Ninja)
  fi
  cmake -E rm -f "${PERF_DIR}/build/CMakeCache.txt"
  cmake -S "${PERF_DIR}" -B "${PERF_DIR}/build" "${generator[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_PREFIX_PATH=${perf_prefix_path}" \
    "-DZLINK_PERF_FRAMEWORK_VERSION=${ZLINK_PERF_FRAMEWORK_VERSION}" \
    -DZLINK_PERF_PACKAGE_SOURCE=local
else
  install_prefix="${PERF_DIR}/.zlink/install"
  # bootstrap.cmake extracts the release asset only into a missing prefix, so a published build never reuses an install.
  cmake -E remove_directory "${install_prefix}"
  cmake -E rm -f "${PERF_DIR}/build/CMakeCache.txt"
  cmake -DZLINK_PROJECT_DIR="${PERF_DIR}" \
    -P "${PERF_DIR}/../samples/bootstrap.cmake"
fi

cmake --build "${PERF_DIR}/build" --target "$1" --parallel "${ZLINK_PERF_BUILD_JOBS:-$(nproc)}"
