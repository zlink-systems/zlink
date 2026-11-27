#!/usr/bin/env bash
set -eo pipefail
package_root="${GITHUB_WORKSPACE}/.artifacts/wsl"
nuget_source="${package_root}/nuget"
mkdir -p "${nuget_source}"
python3 scripts/ci/framework-binding-package.py dotnet "${nuget_source}" \
  -- bash scripts/ci/framework-dotnet-pack-binding.sh --checkout "${nuget_source}" "${1}"
dotnet pack framework/languages/dotnet/src/Zlink.HttpClient/Zlink.HttpClient.csproj -c Release -o "${nuget_source}"
echo "ZLINK_LOCAL_PACKAGE_ROOT=${package_root}" >> "${GITHUB_ENV}"
echo "ZLinkLocalPackageRoot=${package_root}" >> "${GITHUB_ENV}"
