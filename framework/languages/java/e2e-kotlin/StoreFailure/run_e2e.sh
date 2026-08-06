#!/usr/bin/env bash
set -euo pipefail

# Config 6 has historically been named DiscoveryRegistryHa in the Kotlin
# fixture. Keep the canonical suite entry point separate so the aggregate
# runner cannot silently omit the common StoreFailure contract.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LEGACY_RUNNER="${ROOT_DIR}/../DiscoveryRegistryHa/run_e2e.sh"
SCENARIO="${1:-all}"

blocked_common=(
  SF-B3 SF-C3 SF-C4 SF-C5
  SF-F1 SF-F2 SF-F3 SF-F4 SF-F5 SF-F6 SF-F7 SF-F8 SF-F9 SF-F10 SF-F11
  SF-G1 SF-G2 SF-G3
)
supported=(SF-A1 SF-A2 SF-B1 SF-B2 SF-C1 SF-C2 SF-D1 SF-D2 SF-D3 SF-E1)

contains() {
  local expected="$1"
  shift
  local candidate
  for candidate in "$@"; do
    [[ "${candidate}" == "${expected}" ]] && return 0
  done
  return 1
}

if [[ ! -x "${LEGACY_RUNNER}" ]]; then
  echo "Kotlin StoreFailure legacy runner is missing: ${LEGACY_RUNNER}" >&2
  exit 1
fi

if [[ "${SCENARIO}" == "all" ]]; then
  exec "${LEGACY_RUNNER}" all
fi

if contains "${SCENARIO}" "${blocked_common[@]}"; then
  echo "${SCENARIO} is blocked: Kotlin public fixture lacks the required relocation, object-query, cross-language, or capacity control" >&2
  exit 3
fi

if contains "${SCENARIO}" "${supported[@]}"; then
  exec "${LEGACY_RUNNER}" "${SCENARIO}"
fi

echo "unknown Kotlin StoreFailure selector: ${SCENARIO}" >&2
exit 2
