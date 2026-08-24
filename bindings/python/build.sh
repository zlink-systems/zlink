#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"

if [[ "${ZLINK_CORE_SOURCE}" == "release" ]]; then
  export ZLINK_CORE_PREFIX="${ZLINK_CORE_PACKAGE_PREFIX}"
fi
zlink_export_local_core_runtime

cd "${ROOT_DIR}"
exec "${PYTHON_EXECUTABLE:-python3}" -m build "$@"
