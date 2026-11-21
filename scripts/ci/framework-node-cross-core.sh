#!/usr/bin/env bash
set -e
binding_version=$(node -p "require('./framework/languages/node/packages/framework/package.json').dependencies['@zlink-systems/zlink']")
core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
core_prefix=$(bash scripts/local-package/core/fetch-release.sh \
  --version "${core_version}" \
  --platform linux-x64 \
  --cache-dir "${ZLINK_CORE_CACHE_DIR:-.artifacts/core-release}")
{
  echo "ZLINK_CORE_SOURCE=release"
  echo "ZLINK_CORE_PACKAGE_PREFIX=${core_prefix}"
  echo "ZLINK_LIBRARY_PATH=${core_prefix}/lib/libzlink.so.${core_version}"
} >> "${GITHUB_ENV}"
{
  echo "binding_version=${binding_version}"
  echo "prefix=${core_prefix}"
  echo "core_version=${core_version}"
} >> "${GITHUB_OUTPUT}"
