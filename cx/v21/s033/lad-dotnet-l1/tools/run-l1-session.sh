#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet-l1
ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
TOOLS=/root/jobs/fx/cx/v21/s033/r4190m/tools
RUNNER=$ROOT/framework/bench/grpc/dotnet/run_local.sh
SERVER=$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll
CLIENT=$ROOT/framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll
RAW=$ROOT/framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll
FRAMEWORK=$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/Zlink.Framework.dll
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  JOB="$1" ROOT="$2" TOOLS="$3" RUNNER="$4" SERVER="$5" CLIENT="$6" RAW="$7" FRAMEWORK="$8" bash -s <<'"'"'INNER'"'"'
set -euo pipefail
mkdir -p "$JOB/raw/lock-windows"
setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
record(){ local path="$1"; { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq; done; } > "$path"; }
newer(){ test "$1" -nt "$2"; }
setgov performance
trap "setgov powersave" EXIT
record "$JOB/raw/lock-windows/l1-session-before.txt"
for pair in \
  "$SERVER:$ROOT/framework/bench/grpc/dotnet/ZLinkServer/Program.cs" \
  "$CLIENT:$ROOT/framework/bench/grpc/dotnet/Client/Program.cs" \
  "$RAW:$ROOT/framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs" \
  "$FRAMEWORK:$ROOT/framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs" \
  "$FRAMEWORK:$ROOT/framework/languages/dotnet/src/Zlink.Framework/Properties/AssemblyInfo.cs"; do
  dll=${pair%%:*}; src=${pair#*:}; test -f "$dll"; test -f "$src"; newer "$dll" "$src"
done
HEAD=$(git -C "$ROOT" rev-parse HEAD)
DIRTY=$(test -z "$(git -C "$ROOT" status --porcelain)" && echo no || echo yes)
BASE=$(git -C "$ROOT" merge-base HEAD origin/main)
HASHES="server=$(sha256sum "$SERVER" | cut -c1-64);client=$(sha256sum "$CLIENT" | cut -c1-64);framework=$(sha256sum "$FRAMEWORK" | cut -c1-64);binding=$(sha256sum "$RAW" | cut -c1-64)"
parity="receive raw=ZLinkRawServer/Program.cs:26-28,74-80 L1=ZLinkServer/Program.cs:49,160-167; both one 100ms blocking Recv(None), one record, no drain; reply raw=ZLinkRawServer/Program.cs:74-80,103-108 L1=ZLinkServer/Program.cs:169-179,172-174; both new body encode and header, 2-part submit; waits/message raw one Recv, L1 one Recv plus acquire/release state-lane turns; allocations/message same payload wrapper/body/header plus queue lease and async acquire/release operation state; atomics same metrics counters plus L1 lease state exchange; copies both parse once and encode once, no request body reuse"
printf -- "- L1 six-cell cell-set HEAD=%s dirty=%s merge-base=%s DLLs=%s\n- L1 six-cell parity %s\n" "$HEAD" "$DIRTY" "$BASE" "$HASHES" "$parity" >> "$JOB/progress.md"
export PATH="$TOOLS:$PATH" IMPLEMENTATIONS=zlink-framework-dotnet,zlink-dotnet PATTERNS=send-saturation,request-serial,request-backpressure PAYLOAD_SIZES=1024,4096 ZLINK_DOTNET_LADDER_CLIENT=raw SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 WARMUP_SECONDS=30 DURATION_SECONDS=5 RUNS=3
export OUTPUT_DIR="$JOB/raw/l1-fresh" RUN_STAMP=4350-l1-dotnet-main
"$RUNNER" > "$JOB/raw/l1-main-driver.log" 2>&1
mapfile -t failed < <(python3 - "$JOB/raw/l1-fresh" <<'PY'
import glob, json, statistics, sys
root=sys.argv[1]
cells=[('send-saturation',1024),('send-saturation',4096),('request-serial',1024),('request-serial',4096),('request-backpressure',1024),('request-backpressure',4096)]
for pattern,size in cells:
    bad=(pattern=='request-serial' and size==4096)
    for impl in ('zlink-framework-dotnet','zlink-dotnet'):
        paths=[f'{root}/run{i}/{impl}-{pattern}-{size}/results.json' for i in range(1,4)]
        rates=[json.load(open(p))['cells'][0]['throughput_per_second'] for p in paths]
        median=statistics.median(rates)
        spread=max(abs(x-median) for x in rates)/median
        bad |= spread > 0.10
    if bad:
        print(f'{pattern} {size}')
PY
)
for cell in "${failed[@]}"; do
  read -r pattern size <<< "$cell"
  printf -- "- L1 supplement cell=%s-%s HEAD=%s dirty=%s merge-base=%s DLLs=%s\n- L1 supplement parity %s\n" "$pattern" "$size" "$HEAD" "$DIRTY" "$BASE" "$HASHES" "$parity" >> "$JOB/progress.md"
  export PATTERNS="$pattern" PAYLOAD_SIZES="$size" RUNS=2 OUTPUT_DIR="$JOB/raw/l1-fresh-extra/$pattern-$size" RUN_STAMP="4350-l1-dotnet-extra-$pattern-$size"
  "$RUNNER" > "$JOB/raw/l1-extra-$pattern-$size-driver.log" 2>&1
done
record "$JOB/raw/lock-windows/l1-session-after.txt"
INNER
' _ "$JOB" "$ROOT" "$TOOLS" "$RUNNER" "$SERVER" "$CLIENT" "$RAW" "$FRAMEWORK"
