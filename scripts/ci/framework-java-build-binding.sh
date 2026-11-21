#!/usr/bin/env bash
set -euo pipefail
core_version="$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)"
core_prefix="$(bash scripts/local-package/core/fetch-release.sh \
  --version "${core_version}" \
  --platform "${1}" \
  --cache-dir "${ZLINK_CORE_CACHE_DIR:-${RUNNER_TEMP}/zlink-core-release}")"
ZLINK_LOCAL_PACKAGE_ROOT="${GITHUB_WORKSPACE}/${2}" \
  bash scripts/local-package/java/build-wsl.sh \
    --core-prefix "${core_prefix}"
