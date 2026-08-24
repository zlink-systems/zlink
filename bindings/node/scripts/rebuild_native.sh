#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_HELPER="${ROOT_DIR}/../tools/local_core_runtime.sh"
if [[ ! -f "${CORE_HELPER}" ]] || \
    { [[ -n "${ZLINK_CORE_INSTALL_PREFIX:-}" ]] && [[ -z "${ZLINK_CORE_SOURCE:-}" ]]; }; then
  exec node-gyp configure build "$@"
fi

source "${CORE_HELPER}"
exec node-gyp configure build "$@"
