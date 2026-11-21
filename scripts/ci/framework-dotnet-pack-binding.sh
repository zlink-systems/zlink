#!/usr/bin/env bash
set -eo pipefail
binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/dotnet/VERSION)
core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
test -n "${binding_version}" || { echo "::error::binding version is missing" >&2; exit 1; }
test -n "${core_version}" || { echo "::error::Core version is missing" >&2; exit 1; }
core_prefix=$(bash scripts/local-package/core/fetch-release.sh \
  --version "${core_version}" \
  --platform linux-x64 \
  --cache-dir "${ZLINK_CORE_CACHE_DIR:-.artifacts/core-release}")
package_root="${RUNNER_TEMP}/zlink-dotnet-binding"
mkdir -p "${package_root}"
dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj \
  -c Release \
  -o "${package_root}" \
  "-p:ZLinkCoreVersion=${core_version}" \
  "-p:ZLinkCoreSource=local" \
  "-p:ZLinkLinuxX64NativeRoot=${core_prefix}/lib" \
  "-p:ZLinkCoreProvenancePath=${core_prefix}/share/zlink/core-package-provenance.json"
test -s "${package_root}/Zlink.${binding_version}.nupkg" || {
  echo "::error::binding package was not created" >&2
  exit 1
}
artifact_name="zlink-dotnet-binding-${binding_version}"
echo "binding_version=${binding_version}" >> "${GITHUB_OUTPUT}"
echo "artifact_name=${artifact_name}" >> "${GITHUB_OUTPUT}"
