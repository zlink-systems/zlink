#!/usr/bin/env bash
set -euo pipefail
core_version="$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)"
bash scripts/local-package/cpp/build-wsl.sh \
  --core-prefix \
  "${ZLINK_LOCAL_PACKAGE_ROOT}/install/zlink-core/${core_version}"
