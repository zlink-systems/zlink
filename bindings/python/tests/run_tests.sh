#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/../tools/local_core_runtime.sh"
zlink_export_local_core_runtime
PYTHONPATH_ENTRIES=("${ROOT_DIR}/src")
if [[ -n "${PYTHONPATH:-}" ]]; then
  PYTHONPATH_ENTRIES+=("${PYTHONPATH}")
fi
export PYTHONPATH="$(IFS=:; printf '%s' "${PYTHONPATH_ENTRIES[*]}")"
export PYTHONDONTWRITEBYTECODE="${PYTHONDONTWRITEBYTECODE:-1}"
PYTHON_EXECUTABLE="${PYTHON_EXECUTABLE:-python3}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -z "${ZLINK_CORE_PREFIX:-}" ]]; then
  echo "The resolved Core runtime is unavailable; build core/build or provide ZLINK_LIBRARY_PATH." >&2
  exit 2
fi

cd "${ROOT_DIR}"
"${PYTHON_EXECUTABLE}" -m pytest -q "$@"
"${ROOT_DIR}/samples/run_samples.sh"
