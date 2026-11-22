#!/usr/bin/env bash
set -eo pipefail
package_root="${GITHUB_WORKSPACE}/.artifacts/wsl"
nuget_source="${package_root}/nuget"
mkdir -p "${nuget_source}"
binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/dotnet/VERSION)
core_version="${1}"
core_prefix="${2}"
dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj \
  -c Release \
  -o "${nuget_source}" \
  "-p:ZLinkCoreVersion=${core_version}" \
  "-p:ZLinkCoreSource=local" \
  "-p:ZLinkLinuxX64NativeRoot=${core_prefix}/lib" \
  "-p:ZLinkCoreProvenancePath=${core_prefix}/share/zlink/core-package-provenance.json"
test -s "${nuget_source}/Zlink.${binding_version}.nupkg" \
  || { echo "::error::Local Zlink ${binding_version} package was not created" >&2; exit 1; }
dotnet pack framework/languages/dotnet/src/Zlink.HttpClient/Zlink.HttpClient.csproj -c Release -o "${nuget_source}"
echo "ZLINK_LOCAL_PACKAGE_ROOT=${package_root}" >> "${GITHUB_ENV}"
echo "ZLinkLocalPackageRoot=${package_root}" >> "${GITHUB_ENV}"
