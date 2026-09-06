#!/usr/bin/env bash
# Source from the repository checkout; package content selects an isolated NuGet cache.
PERF_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF_REPO_ROOT="$(cd "${PERF_SCRIPT_DIR}/../../../../.." && pwd)"
export TMPDIR=/dev/shm/zlink-tmp-dotnet
export ZLINK_LIBRARY_PATH="${PERF_REPO_ROOT}/core/build-dev/lib"
export UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
pkg_hash="$(sha256sum "${PERF_REPO_ROOT}/.artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg" | awk '{print $1}')"
export NUGET_PACKAGES="/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}"
mkdir -p "${TMPDIR}" "${NUGET_PACKAGES}"
