#!/usr/bin/env bash
# Shared helpers for the serialized framework gates. Source this file.
Z="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TAG="${1:?usage: <script> <tag>}"
# Core runtime the gates run against. Default is the dev build; a release check sets
# ZLINK_GATE_CORE_LIB to a release prefix lib dir (e.g. ~/.cache/zlink/core/0.17.3/linux-x64/lib).
CORE_LIB="${ZLINK_GATE_CORE_LIB:-$Z/core/build-dev/lib}"
CORE_VER="${ZLINK_GATE_CORE_VERSION:-$(sed -n 's/^LIBZLINK_VERSION=//p' "$Z/VERSION")}"
BINDING_VER="${ZLINK_GATE_BINDING_VERSION:-$(sed -n 's/^ZLINK_BINDINGS_VERSION=//p' "$Z/BINDINGS_VERSION")}"
LOGS="$Z/zlink-work/gates/$TAG"; mkdir -p "$LOGS"
ts() { date +%H:%M; }
require_quiet() { # gates carry timing assertions; refuse to start on a loaded box
  local max="${ZLINK_GATE_MAX_LOAD:-10}" load; load=$(cut -d' ' -f1 /proc/loadavg)
  awk -v l="$load" -v m="$max" 'BEGIN{exit !(l>m)}' && { echo "load average $load > $max; wait or set ZLINK_GATE_MAX_LOAD" >&2; return 1; }; return 0
}
run() { # name dir cmd... — serialized behind the samples lock, log per step
  local name=$1 dir=$2; shift 2
  echo "[$(ts)] $name start"
  ( cd "$Z/$dir" && flock -w7200 /tmp/zlink-samples-gate.lock "$@" ) > "$LOGS/$name.log" 2>&1
  local rc=$?; echo "[$(ts)] $name exit=$rc"; echo "$name $rc" >> "$LOGS/results.txt"; return $rc
}
dotnet_env() {
  export TMPDIR=/dev/shm/zlink-tmp-dotnet ZLINK_LIBRARY_PATH="$CORE_LIB" ZLINK_LOCAL_PACKAGE_ROOT="$Z/.artifacts/wsl" UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
  local h; h=$(sha256sum "$Z/.artifacts/wsl/nuget/Systems.Zlink.$BINDING_VER.nupkg" | awk '{print $1}'); export NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${h:0:16}; mkdir -p "$TMPDIR"
}
