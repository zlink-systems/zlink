#!/usr/bin/env bash
set -eo pipefail
# The framework consumes the public binding package contract. During
# pre-release CI the package is packed from this checkout against the
# verified Core release instead of waiting for nuget.org publication.
package_root="${RUNNER_TEMP}/zlink-local-packages"
nuget_source="${package_root}/nuget"
mkdir -p "${nuget_source}"
binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/dotnet/VERSION)
if [ -z "${binding_version}" ]; then
  echo "::error::Zlink binding version not found in bindings/dotnet/VERSION" >&2
  exit 1
fi
binding_artifact="${1}"
test -n "${binding_artifact}" || {
  echo "::error::binding artifact name is missing" >&2
  exit 1
}
test -s "${nuget_source}/Zlink.${binding_version}.nupkg" \
  || { echo "::error::Downloaded Zlink ${binding_version} package is missing" >&2; exit 1; }
dotnet pack framework/languages/dotnet/src/Zlink.HttpClient/Zlink.HttpClient.csproj -c Release -o "${nuget_source}"
echo "ZLINK_LOCAL_PACKAGE_ROOT=${package_root}" >> "${GITHUB_ENV}"
echo "ZLinkLocalPackageRoot=${package_root}" >> "${GITHUB_ENV}"
echo "binding_version=${binding_version}" >> "${GITHUB_OUTPUT}"
