#!/usr/bin/env bash
set -euo pipefail
rss_log="${RUNNER_TEMP}/framework-cpp-tu-rss.tsv"
cxx_launcher="${ZLINK_SCCACHE_LAUNCHER}"
: > "${rss_log}"
# sccache runs the compiler in its server process, outside GNU time's process
# tree, so compile RSS is recorded only for builds without sccache (pre-pr).
if [[ -n "${ZLINK_SCCACHE_LAUNCHER}" ]]; then
  echo "C++ compile RSS is not recorded: sccache runs the compiler outside the launcher."
elif [[ -x /usr/bin/time ]]; then
  cxx_launcher="bash;${GITHUB_WORKSPACE}/scripts/ci/framework-cpp-tu-launcher.sh;${rss_log}"
else
  echo "::warning::C++ compile RSS recording requires GNU time."
fi

cmake --preset linux-ninja-release \
  -DVCPKG_MANIFEST_FEATURES=etcd \
  -DVCPKG_TARGET_TRIPLET="${ZLINK_VCPKG_TRIPLET}" \
  -DVCPKG_HOST_TRIPLET="${ZLINK_VCPKG_TRIPLET}" \
  -DVCPKG_OVERLAY_TRIPLETS="${GITHUB_WORKSPACE}/framework/languages/cpp/cmake/vcpkg-triplets" \
  -DZLINK_FRAMEWORK_CPP_WITH_ETCD=ON \
  -DZLINK_FRAMEWORK_CPP_TEST_PROBES=ON \
  -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER="${ZLINK_SCCACHE_LAUNCHER}" \
  -DCMAKE_CXX_COMPILER_LAUNCHER="${cxx_launcher}" \
  -DCMAKE_TOOLCHAIN_FILE="${GITHUB_WORKSPACE}/.github/workflows/framework-cpp-system-toolchain.cmake" \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
cmake --build --preset linux-ninja-release --parallel "${BUILD_PARALLEL:-2}"
if [[ -n "${ZLINK_SCCACHE_LAUNCHER}" ]]; then
  sccache --show-stats
fi
