#!/usr/bin/env bash
set -euo pipefail

if (($# > 1)); then
  echo "Usage: $0 [core-version]" >&2
  exit 2
fi

if (($# == 1)); then
  core_version="$1"
else
  core_version="$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)"
fi

if ! [[ "$core_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "::error::Invalid or missing LIBZLINK_VERSION in VERSION: ${core_version}" >&2
  exit 1
fi

expected_tag="core/v${core_version}"
if ! is_draft="$(gh release view "$expected_tag" --repo zlink-systems/zlink --json isDraft --jq '.isDraft')"; then
  echo "::error::Expected published Core release ${expected_tag}, but it does not exist." >&2
  exit 1
fi

if [[ "$is_draft" != "false" ]]; then
  echo "::error::Expected published Core release ${expected_tag}, but it is a draft." >&2
  exit 1
fi

echo "Found published Core release: ${expected_tag}"
