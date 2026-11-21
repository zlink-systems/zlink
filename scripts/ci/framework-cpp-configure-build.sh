#!/usr/bin/env bash
set -euo pipefail
cmake --preset linux-ninja-release \
  -DVCPKG_MANIFEST_FEATURES=etcd \
  -DVCPKG_TARGET_TRIPLET="${ZLINK_VCPKG_TRIPLET}" \
  -DVCPKG_HOST_TRIPLET="${ZLINK_VCPKG_TRIPLET}" \
  -DVCPKG_OVERLAY_TRIPLETS="${GITHUB_WORKSPACE}/framework/languages/cpp/cmake/vcpkg-triplets" \
  -DZLINK_FRAMEWORK_CPP_WITH_ETCD=ON \
  -DZLINK_FRAMEWORK_CPP_TEST_PROBES=ON \
  -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER="${ZLINK_SCCACHE_LAUNCHER}" \
  -DCMAKE_CXX_COMPILER_LAUNCHER="${ZLINK_SCCACHE_LAUNCHER}" \
  -DCMAKE_TOOLCHAIN_FILE="${GITHUB_WORKSPACE}/.github/workflows/framework-cpp-system-toolchain.cmake" \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
cmake --build --preset linux-ninja-release --parallel "${BUILD_PARALLEL:-2}"
if [[ -n "${ZLINK_SCCACHE_LAUNCHER}" ]]; then
  sccache --show-stats
fi
