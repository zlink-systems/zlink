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
cmake -S "${HERE}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_MANIFEST_DIR="${REPO_ROOT}/framework/languages/cpp" \
  -DVCPKG_MANIFEST_FEATURES=bench \
  -DVCPKG_OVERLAY_PORTS="${REPO_ROOT}/vcpkg/ports"
cmake --build "${BUILD_DIR}" --parallel 2
