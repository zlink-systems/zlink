#!/usr/bin/env bash
# Builds the .NET with-grpc bench. Owner of the .NET build command; run_local.sh and build_all.sh call it.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${ROOT_DIR}/../runner_common.sh"

# MSBuild node reuse leaves build servers running after the build; they inherit the caller's
# file descriptors, so a perf lock taken around the build stays held and the next measurement
# waits forever.
export MSBUILDDISABLENODEREUSE=1
bench_require_low_load
dotnet build "${ROOT_DIR}/WithGrpcBench.sln" -c Release

# A Release app can copy a Debug Framework ProjectReference unless the reference carries its
# configuration. Verify every copied local-source Framework assembly against the Release output.
if [[ "${ZLinkSampleUseLocalSource:-true}" != [fF][aA][lL][sS][eE] ]]; then
  FRAMEWORK_SRC="${ROOT_DIR}/../../../languages/dotnet/src"
  framework_outputs=0
  while IFS= read -r -d '' output; do
    framework_outputs=$((framework_outputs + 1))
    case "${output}" in
      */bin/Release/*) ;;
      *) echo "Framework assembly is outside the bench Release output: ${output}" >&2; exit 1 ;;
    esac
    release_source="$(find "${FRAMEWORK_SRC}" -type f -path '*/bin/Release/*' -name "$(basename "${output}")" -print -quit)"
    if [[ -z "${release_source}" ]] || ! cmp -s "${release_source}" "${output}"; then
      echo "Framework assembly does not match its Release project output: ${output}" >&2
      exit 1
    fi
    echo "Verified Release Framework assembly: ${output} <- ${release_source}"
  done < <(find "${ROOT_DIR}" -type f -path '*/bin/*' -name 'Zlink.Framework*.dll' -print0)
  if ((framework_outputs == 0)); then
    echo "No copied Framework assemblies found in the .NET bench outputs" >&2
    exit 1
  fi
fi
