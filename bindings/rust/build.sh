#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"

if [[ "${ZLINK_CORE_SOURCE}" == "local" ]]; then
  export ZLINK_RUST_NATIVE_DIR="${ZLINK_CORE_LIB_DIR}"
fi
export LD_LIBRARY_PATH="${ZLINK_CORE_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

cd "${ROOT_DIR}"
exec cargo build "$@"
