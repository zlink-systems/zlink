#!/usr/bin/env bash
set -eo pipefail
# The released binding package carries the linux-x64 runtime only; the
# other RIDs run the tests against the verified Core release archive.
# The binding package version and the Core release it embeds differ; VERSION owns Core.
core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
core_prefix=$(bash scripts/local-package/core/fetch-release.sh \
  --version "${core_version}" \
  --platform "${1}" \
  --cache-dir "${ZLINK_CORE_CACHE_DIR:-.artifacts/core-release}")
case "${1}" in
  windows-*) library="${core_prefix}/bin/zlink.dll" ;;
  macos-*) library="${core_prefix}/lib/libzlink.dylib" ;;
  *) library="${core_prefix}/lib/libzlink.so.${core_version}" ;;
esac
test -f "${library}" || { echo "::error::Core runtime not found at ${library}" >&2; ls -R "${core_prefix}" >&2; exit 1; }
if [ "${RUNNER_OS}" = "Windows" ]; then
  library=$(cygpath -w "${library}")
fi
echo "ZLINK_LIBRARY_PATH=${library}" >> "${GITHUB_ENV}"
