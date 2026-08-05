#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHONPATH_ENTRIES=("${ROOT_DIR}/src")
if [[ -n "${PYTHONPATH:-}" ]]; then
  PYTHONPATH_ENTRIES+=("${PYTHONPATH}")
fi
export PYTHONPATH="$(IFS=:; printf '%s' "${PYTHONPATH_ENTRIES[*]}")"
export PYTHONDONTWRITEBYTECODE="${PYTHONDONTWRITEBYTECODE:-1}"
PYTHON_EXECUTABLE="${PYTHON_EXECUTABLE:-python3}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -z "${ZLINK_CORE_PREFIX:-}" ]]; then
  echo "Set ZLINK_LIBRARY_PATH or ZLINK_CORE_PREFIX before running Python binding tests" >&2
  exit 2
fi

cd "${ROOT_DIR}"
"${PYTHON_EXECUTABLE}" -m pytest -q "$@"
"${ROOT_DIR}/samples/run_samples.sh"
