#!/usr/bin/env bash
set -euo pipefail
label="$1"; runner="$2"; output="$3"; shift 3
JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b
TOOLS=/root/jobs/fx/cx/v21/s033/r4190m/tools
mkdir -p "$JOB/raw/lock-windows"
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  label="$1"; runner="$2"; output="$3"; shift 3
  setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
  record(){ local path="$1"; { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq; done; } > "$path"; }
  trap "setgov powersave" EXIT
  setgov performance
  record "/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/raw/lock-windows/${label}-before.txt"
  ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
  HEAD=$(git -C "$ROOT" rev-parse HEAD)
  DIRTY=$(test -z "$(git -C "$ROOT" status --porcelain)" && echo no || echo yes)
  BASE=$(git -C "$ROOT" merge-base HEAD origin/main)
  SERVER="$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll"
  CLIENT="$ROOT/framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll"
  RAW="$ROOT/framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll"
  HASHES="server=$(sha256sum "$SERVER" | cut -c1-64);client=$(sha256sum "$CLIENT" | cut -c1-64);binding=$(sha256sum "$RAW" | cut -c1-64)"
  IFS=, read -r -a cells <<< "${PROVENANCE_CELLS:-send-saturation-1024,send-saturation-4096,request-serial-1024,request-serial-4096,request-backpressure-1024,request-backpressure-4096}"
  for cell in "${cells[@]}"; do
    printf -- "- %s cell=%s HEAD=%s dirty=%s merge-base=%s DLLs=%s\n" "$label" "$cell" "$HEAD" "$DIRTY" "$BASE" "$HASHES" >> /root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/progress.md
  done
  export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH"
  export WARMUP_SECONDS="${WARMUP_SECONDS:-30}" DURATION_SECONDS="${DURATION_SECONDS:-5}" RUNS="${RUNS:-3}" SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
  export OUTPUT_DIR="$output" RUN_STAMP="${RUN_STAMP_PREFIX:-4350-l0e}-${label}"
  "$runner" >"/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/raw/${label}-driver.log" 2>&1
  record "/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/raw/lock-windows/${label}-after.txt"
' _ "$label" "$runner" "$output" "$@"
