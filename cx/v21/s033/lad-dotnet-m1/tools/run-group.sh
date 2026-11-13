#!/usr/bin/env bash
set -euo pipefail

JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet-m1
ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
RUNNER="$ROOT/framework/bench/grpc/dotnet/run_local.sh"
TOOLS=/root/jobs/fx/cx/v21/s033/r4190m/tools
if [[ "${M1_LOCKED:-0}" != 1 ]]; then
  exec flock -x /root/jobs/fx/cx/measure.lock env M1_LOCKED=1 "$0" "$@"
fi

HOLD_START=$SECONDS
mkdir -p "$JOB/raw/lock-windows"
setgov() {
  for cpu in $(seq 0 15); do
    echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"
  done
}
record() {
  local path="$1"
  { date -u +%FT%TZ; uptime; free -b; } > "$path"
}
trap 'setgov powersave' EXIT
setgov performance
record "$JOB/raw/lock-windows/session-before.txt"

SERVER="$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll"
CLIENT="$ROOT/framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll"
RAW="$ROOT/framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll"
FRAMEWORK="$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/Zlink.Framework.dll"
HEAD=$(git -C "$ROOT" rev-parse HEAD)
DIRTY=$(test -z "$(git -C "$ROOT" status --porcelain)" && echo no || echo yes)
BASE=$(git -C "$ROOT" merge-base HEAD origin/main)
HASHES="server=$SERVER@$(sha256sum "$SERVER" | cut -d' ' -f1);client=$CLIENT@$(sha256sum "$CLIENT" | cut -d' ' -f1);raw=$RAW@$(sha256sum "$RAW" | cut -d' ' -f1);framework=$FRAMEWORK@$(sha256sum "$FRAMEWORK" | cut -d' ' -f1)"
CELLS=(send-saturation-1024 send-saturation-4096 request-serial-1024 request-serial-4096 request-backpressure-1024 request-backpressure-4096)

record_progress() {
  local label="$1" impl="$2" output="$3" cell="$4" pattern="${4%-*}" size="${4##*-}" receive extra
  if [[ "$label" == binding ]]; then
    receive="binding=ZLinkRawServer/Program.cs:64-76,119-137; ladder=not applicable"
    extra="wait=source completion/admission + one blocking target Recv; allocations=source header/body Messages + target payload/reply body/header; atomics=existing metrics only; copies=one request protobuf write + one reply protobuf write"
  elif [[ "$label" == L0* ]]; then
    receive="binding=ZLinkRawServer/Program.cs:64-76,119-137; ladder=ZLinkServer/Program.cs:140-159"
    extra="wait=source completion/admission + one blocking target Recv; allocations=source header/body Messages + matching target payload/reply body/header; atomics=existing metrics only; copies=one request protobuf write + one reply protobuf write"
  elif [[ "$label" == L1* || "$label" == L2 ]]; then
    receive="binding=ZLinkRawServer/Program.cs:64-76,119-137; ladder=ZLinkServer/Program.cs:198-217"
    extra="wait=source completion/admission + one permit acquire + one blocking Recv; allocations=source header/body Messages + queue lease + matching target payload/reply body/header; atomics=existing metrics plus one lease Interlocked.Exchange; copies=one request protobuf write + one reply protobuf write"
    if [[ "$label" == L2 ]]; then
      receive+=" + one ZLinkEnvelopeCodec.DecodeHeader at ZLinkServer/Program.cs:200-203"
      extra+="; source encodes one Framework header (request correlation counter Interlocked.Increment + deadline) and target decodes it once; protobuf body copies unchanged"
    fi
  fi
  printf -- '- %s %s HEAD=%s dirty=%s merge-base=%s binaries=%s output=%s cell=%s receive{%s} reply{binding=ZLinkRawServer/Program.cs:74-85,103-109; ladder=ZLinkServer/Program.cs:148-159,205-217; new protobuf body + raw response header + two-part submit} costs{%s}\n' \
    "$label" "$impl" "$HEAD" "$DIRTY" "$BASE" "$HASHES" "$output" "$cell" "$receive" "$extra" >> "$JOB/progress.md"
}

run_one() {
  local label="$1" impl="$2" pattern="$3" size="$4" output="$5" level="$6"
  local attempt=0 base result rate
  base="$output"
  while :; do
    if ((SECONDS - HOLD_START >= 1500)); then
      echo "measurement hold reached 25-minute safety boundary" >&2
      record "$JOB/raw/lock-windows/session-after.txt"
      exit 124
    fi
    mkdir -p "$base"
    record_progress "$label" "$impl" "$base" "$pattern-$size"
    export PATH="$TOOLS:$PATH" IMPLEMENTATIONS="$impl" PATTERNS="$pattern" PAYLOAD_SIZES="$size"
    export SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
    export WARMUP_SECONDS=10 DURATION_SECONDS=5 RUNS=1 OUTPUT_DIR="$base" SCHEDULING_RECORD=1
    export RUN_STAMP="4350-dotnet-${label}-${pattern}-${size}-$(date -u +%Y%m%dT%H%M%SZ)"
    export ZLINK_DOTNET_LADDER_LEVEL="$level"
    if [[ "$impl" == zlink-framework-dotnet ]]; then
      export ZLINK_DOTNET_LADDER_CLIENT=raw
    else
      unset ZLINK_DOTNET_LADDER_CLIENT || true
    fi
    "$RUNNER" > "$JOB/raw/${label}-${pattern}-${size}-attempt${attempt}-driver.log" 2>&1
    result="$base/run1/${impl}-${pattern}-${size}/results.json"
    if [[ -f "${result}.scheduling-invalid" ]]; then
      printf -- '- invalid run discarded; non-SCHED_OTHER policy: %s\n' "$result" >> "$JOB/progress.md"
      attempt=$((attempt + 1))
      base="$JOB/raw/${label}-retry/${pattern}-${size}/attempt${attempt}"
      continue
    fi
    rate=$(python3 - "$result" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
cell = json.loads(path.read_text())['cells'][0]
assert cell['errors'] == 0 and cell['server_errors'] == 0 and cell['target_stats']['errors'] == 0
assert cell['submitted'] == cell['completed'] == cell['target_stats']['received']
schedule = pathlib.Path(str(path) + '.scheduling.txt').read_text()
assert 'phase=process-start' in schedule and 'phase=mid-active' in schedule
assert 'discovery-failed' not in schedule
policies = [line for line in schedule.splitlines() if 'current scheduling policy:' in line]
assert policies and all('current scheduling policy: SCHED_OTHER' in line for line in policies)
print(cell['throughput_per_second'])
PY
)
    printf -- '- result %s %s msg/s=%s raw=%s schedule=%s.scheduling.txt\n' \
      "$label" "$pattern-$size" "$rate" "$result" "$result" >> "$JOB/progress.md"
    break
  done
}

run_variant() {
  local label="$1" impl="$2" level="$3" pattern size output
  for cell in "${CELLS[@]}"; do
    pattern=${cell%-*}; size=${cell##*-}
    output="$JOB/raw/$label/$cell"
    run_one "$label" "$impl" "$pattern" "$size" "$output" "$level"
  done
}

TODAY=$(date -u +%F)
BINDING_DAILY="/root/jobs/fx/cx/v21/s033/binding-daily/dotnet-${TODAY}.json"
if [[ -f "$BINDING_DAILY" ]]; then
  printf -- '- binding reference reused: %s\n' "$BINDING_DAILY" >> "$JOB/progress.md"
else
  run_variant binding zlink-dotnet L0
fi
run_variant L0 zlink-framework-dotnet L0
run_variant L1 zlink-framework-dotnet L1
run_variant L2 zlink-framework-dotnet L2

# The request-serial 4 KiB comparison was the prior boundary cell. The initial
# one-run result plus two paired runs make a three-run confirmation.
for level in L0 L1; do
  for replicate in 2 3; do
    output="$JOB/raw/${level}-request-serial-4096-confirm/run${replicate}"
    run_one "${level}-confirm" zlink-framework-dotnet request-serial 4096 "$output" "$level"
  done
done
record "$JOB/raw/lock-windows/session-after.txt"
