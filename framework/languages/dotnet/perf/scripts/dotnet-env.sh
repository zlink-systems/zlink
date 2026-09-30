#!/usr/bin/env bash
# Source from the perf entry scripts. Packages restore from nuget.org into the default NuGet cache.
export TMPDIR=/dev/shm/zlink-tmp-dotnet
export UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
mkdir -p "${TMPDIR}"
