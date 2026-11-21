#!/usr/bin/env bash
set -eo pipefail
binding_version=$(node -p "require('./framework/languages/node/packages/framework/package.json').dependencies['@zlink-systems/zlink']")
# The binding package version and the Core release it embeds differ
# (binding 0.17.6 ships Core 0.17.5); the Core version is owned by VERSION.
core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
core_prefix=$(bash scripts/local-package/core/fetch-release.sh \
  --version "${core_version}" \
  --platform "${1}" \
  --cache-dir "${ZLINK_CORE_CACHE_DIR:-.artifacts/core-release}")
if [ "${RUNNER_OS}" = "Windows" ]; then
  core_prefix=$(cygpath -w "${core_prefix}")
  # The binding's Windows loader searches ZLINK_CORE_PREFIX\bin.
  echo "ZLINK_CORE_PREFIX=${core_prefix}" >> "${GITHUB_ENV}"
  printf '%s\n' "${core_prefix}\\bin" >> "${GITHUB_PATH}"
fi
echo "ZLINK_CORE_SOURCE=release" >> "${GITHUB_ENV}"
echo "ZLINK_CORE_PACKAGE_PREFIX=${core_prefix}" >> "${GITHUB_ENV}"
echo "prefix=${core_prefix}" >> "${GITHUB_OUTPUT}"
echo "binding_version=${binding_version}" >> "${GITHUB_OUTPUT}"
