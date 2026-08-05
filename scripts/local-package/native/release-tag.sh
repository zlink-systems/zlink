#!/usr/bin/env bash
# Shared parser for Core release references. A release tag may carry an RC
# suffix, while the runtime ABI and public version markers remain numeric.

zlink_parse_core_release_ref() {
  if [[ "$#" -ne 1 || -z "$1" ]]; then
    echo "Error: Core release reference is required." >&2
    return 2
  fi

  local ref="$1"
  local tag
  if [[ "${ref}" == *"/releases/tag/"* ]]; then
    tag="${ref#*/releases/tag/}"
  else
    tag="${ref}"
  fi

  # GitHub may render the slash in a tag either literally or URL-encoded.
  tag="${tag//%2F//}"
  tag="${tag//%2f//}"

  if [[ ! "${tag}" =~ ^core/v([0-9]+\.[0-9]+\.[0-9]+)(-rc\.([1-9][0-9]*))?$ ]]; then
    echo "Error: release tag must use core/vX.Y.Z or core/vX.Y.Z-rc.N exactly: ${tag}" >&2
    return 2
  fi

  ZLINK_CORE_RELEASE_TAG="${tag}"
  ZLINK_CORE_RUNTIME_VERSION="${BASH_REMATCH[1]}"
  if [[ -n "${BASH_REMATCH[2]}" ]]; then
    ZLINK_CORE_RELEASE_CHANNEL="rc"
    ZLINK_CORE_RELEASE_SEQUENCE="${BASH_REMATCH[3]}"
  else
    ZLINK_CORE_RELEASE_CHANNEL="stable"
    ZLINK_CORE_RELEASE_SEQUENCE=""
  fi
  ZLINK_CORE_RELEASE_TAG_ENCODED="${tag//\//%2F}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  set -euo pipefail
  zlink_parse_core_release_ref "${1:-}"
  printf 'tag=%s\n' "${ZLINK_CORE_RELEASE_TAG}"
  printf 'runtime_version=%s\n' "${ZLINK_CORE_RUNTIME_VERSION}"
  printf 'channel=%s\n' "${ZLINK_CORE_RELEASE_CHANNEL}"
  printf 'sequence=%s\n' "${ZLINK_CORE_RELEASE_SEQUENCE}"
  printf 'encoded_tag=%s\n' "${ZLINK_CORE_RELEASE_TAG_ENCODED}"
fi
