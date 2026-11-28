#!/usr/bin/env bash
set -euo pipefail
binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/dotnet/VERSION)
core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
test -n "${binding_version}" || { echo "::error::binding version is missing" >&2; exit 1; }
test -n "${core_version}" || { echo "::error::Core version is missing" >&2; exit 1; }

if [[ "${1:-}" == "--checkout" ]]; then
  package_root="${2}"
  core_prefix="${3:-}"
  if [[ -z "${core_prefix}" ]]; then
    core_prefix=$(python3 scripts/ci/framework_timing.py "Core release fetch for .NET binding" \
      bash scripts/local-package/core/fetch-release.sh \
      --version "${core_version}" \
      --platform linux-x64 \
      --cache-dir "${ZLINK_CORE_CACHE_DIR:-.artifacts/core-release}")
  fi
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
  exit 0
fi

package_root="${RUNNER_TEMP}/zlink-dotnet-binding"
python3 scripts/ci/framework-binding-package.py dotnet "${package_root}" \
  -- bash "$0" --checkout "${package_root}"
echo "binding_version=${binding_version}" >> "${GITHUB_OUTPUT}"
echo "artifact_name=zlink-dotnet-binding-${binding_version}" >> "${GITHUB_OUTPUT}"
