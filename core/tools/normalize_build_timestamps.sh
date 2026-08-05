#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-}"
if [[ -z "${BUILD_DIR}" ]]; then
  echo "usage: $0 <build-dir>" >&2
  exit 1
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  exit 0
fi

STAMP_FILE="$(mktemp /tmp/zlink-normalize-build-timestamps.XXXXXX)"
trap 'rm -f "${STAMP_FILE}"' EXIT
touch "${STAMP_FILE}"

future_count="$(
  find "${BUILD_DIR}" -type f -newer "${STAMP_FILE}" -print | wc -l
)"
if [[ "${future_count}" -eq 0 ]]; then
  exit 0
fi

find "${BUILD_DIR}" -type f -newer "${STAMP_FILE}" -exec touch -c -r "${STAMP_FILE}" {} +
echo "Normalized ${future_count} future-dated file(s) in ${BUILD_DIR}"
