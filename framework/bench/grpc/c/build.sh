#!/usr/bin/env bash
# Builds the C with-grpc bench. Owner of the C build command; run_local.sh and build_all.sh call it.
# Inputs (optional): BUILD_DIR, ZLINK_CORE_PACKAGE_PREFIX (Core prefix; default: the local
# package Core under ZLINK_LOCAL_PACKAGE_ROOT), ZLINK_LOCAL_PACKAGE_ROOT, ZLINK_C_CORE_VERSION.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../../../.." && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"

# README §7.2 formula 1 is `zlink-<lang> / zlink-c`. The language rows load the Core of the
# local package, so the C baseline builds against the same binary; the `core/build` development
# build differs by about 6% (#308: 600.2 vs 636.6 KOPS, latency 13.1 vs 8.1 ms).
ZLINK_LOCAL_PACKAGE_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-${REPO_ROOT}/.artifacts/wsl}"
ZLINK_C_CORE_VERSION="${ZLINK_C_CORE_VERSION:-$(sed -n 's/^LIBZLINK_VERSION=//p' "${REPO_ROOT}/VERSION")}"
ZLINK_C_CORE_DEFAULT_PREFIX="${ZLINK_LOCAL_PACKAGE_ROOT}/install/zlink-core/${ZLINK_C_CORE_VERSION}"
BUILD_DIR="${BUILD_DIR:-${HERE}/build}"

if [[ ! -d "${ZLINK_C_CORE_DEFAULT_PREFIX}" && -z "${ZLINK_CORE_PACKAGE_PREFIX:-}" ]]; then
  echo "C 기준 벤치가 쓸 Core 패키지가 없다: ${ZLINK_C_CORE_DEFAULT_PREFIX}" >&2
  echo "ZLINK_CORE_PACKAGE_PREFIX로 명시하거나 로컬 패키지를 먼저 만들어라." >&2
  exit 1
fi
bench_require_low_load
cmake -S "${HERE}" -B "${BUILD_DIR}" \
  -DZLINK_C_CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX:-${ZLINK_C_CORE_DEFAULT_PREFIX}}"
cmake --build "${BUILD_DIR}" --target \
  bench_c_with_grpc_zlink_server bench_c_with_grpc_zlink_client \
  bench_c_with_grpc_grpc_server bench_c_with_grpc_grpc_client -j4
