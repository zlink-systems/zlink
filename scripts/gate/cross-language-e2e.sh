#!/usr/bin/env bash
# Cross-language E2E (cpp all-stage, node smoke, java-cross) on the current packages, serialized behind the language locks.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"; cd "$Z"; require_quiet || exit 2
step() { echo "[$(ts)] $1 exit=$2"; echo "$1 $2" >> "$LOGS/results.txt"; }
: > "$LOGS/results.txt"
( cd framework/languages/cpp && cmake --build build/linux-ninja-c-e2e -j4 ) > "$LOGS/cpp-c-e2e-build.log" 2>&1; step cpp-c-e2e-build $?
( cd framework/languages/node && TMPDIR=/dev/shm/zlink-tmp-node flock -w7200 /tmp/zlink-node-gate.lock npm run build ) > "$LOGS/node-build.log" 2>&1; step node-build $?
export ZLINK_CPP_BUILD_DIR="$Z/framework/languages/cpp/build/linux-ninja-c-e2e"
unset ZLINK_LIBRARY_PATH
export UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
pkg_hash=$(sha256sum .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg | awk '{print $1}'); export NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}
TMPDIR=/dev/shm/zlink-tmp-dotnet flock -w7200 /tmp/zlink-samples-gate.lock flock -w7200 /tmp/zlink-dotnet-gate.lock flock -w7200 /tmp/zlink-node-gate.lock flock -w7200 /tmp/zlink-jvm-gate.lock framework/languages/cpp/cross-language/run_cross_language_smoke.sh > "$LOGS/cpp-all-stage.log" 2>&1; step cpp-all-stage $?
TMPDIR=/dev/shm/zlink-tmp-node flock -w7200 /tmp/zlink-samples-gate.lock flock -w7200 /tmp/zlink-node-gate.lock flock -w7200 /tmp/zlink-dotnet-gate.lock framework/languages/node/cross-language/run_cross_language_smoke.sh > "$LOGS/node-smoke.log" 2>&1; step node-smoke $?
TMPDIR=/dev/shm/zlink-tmp-java ZLINK_CPP_CROSS_LANGUAGE_STAGE=java-cross flock -w7200 /tmp/zlink-samples-gate.lock flock -w7200 /tmp/zlink-jvm-gate.lock flock -w7200 /tmp/zlink-node-gate.lock flock -w7200 /tmp/zlink-dotnet-gate.lock framework/languages/cpp/cross-language/run_cross_language_smoke.sh > "$LOGS/java-cross.log" 2>&1; step java-cross $?
echo "XLANG_DONE $TAG"; awk '$2!=0{f=1; print "FAILED:", $1} END{exit f}' "$LOGS/results.txt" && echo "ALL GREEN"
touch "$LOGS/xlang.done"
