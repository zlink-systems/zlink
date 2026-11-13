#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet-m1
ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
RUNNER="$ROOT/framework/bench/grpc/dotnet/run_local.sh"
TOOLS=/root/jobs/fx/cx/v21/s033/r4190m/tools
if [[ "${M1_LOCKED:-0}" != 1 ]]; then
  exec flock -x /root/jobs/fx/cx/measure.lock env M1_LOCKED=1 "$0" "$@"
fi
(($# % 4 == 0 && $# > 0)) || { echo 'arguments: previous-level new-level pattern payload-size [repeat ...]' >&2; exit 2; }
HOLD_START=$SECONDS
mkdir -p "$JOB/raw/lock-windows"
setgov() { for cpu in $(seq 0 15); do echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; done; }
record() { { date -u +%FT%TZ; uptime; free -b; } > "$1"; }
trap 'setgov powersave' EXIT
setgov performance
record "$JOB/raw/lock-windows/boundary-confirm-before.txt"
SERVER="$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll"
CLIENT="$ROOT/framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll"
RAW="$ROOT/framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll"
FRAMEWORK="$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/Zlink.Framework.dll"
HEAD=$(git -C "$ROOT" rev-parse HEAD)
DIRTY=$(test -z "$(git -C "$ROOT" status --porcelain)" && echo no || echo yes)
BASE=$(git -C "$ROOT" merge-base HEAD origin/main)
HASHES="server=$SERVER@$(sha256sum "$SERVER" | cut -d' ' -f1);client=$CLIENT@$(sha256sum "$CLIENT" | cut -d' ' -f1);raw=$RAW@$(sha256sum "$RAW" | cut -d' ' -f1);framework=$FRAMEWORK@$(sha256sum "$FRAMEWORK" | cut -d' ' -f1)"

while (($#)); do
  previous="$1"; new="$2"; pattern="$3"; size="$4"; shift 4
  for level in "$previous" "$new"; do
    for replicate in 2 3; do
      if ((SECONDS - HOLD_START >= 1500)); then
        echo 'boundary hold reached 25-minute safety boundary' >&2
        record "$JOB/raw/lock-windows/boundary-confirm-after.txt"
        exit 124
      fi
      impl=zlink-framework-dotnet
      output="$JOB/raw/boundary-confirm/${previous}-${new}/${pattern}-${size}/run${replicate}/${level}"
      cell="$pattern-$size"
      printf -- '- boundary-confirm %s->%s level=%s cell=%s run=%s HEAD=%s dirty=%s merge-base=%s binaries=%s output=%s receive{binding=ZLinkRawServer/Program.cs:64-76,119-137; ladder=ZLinkServer/Program.cs:%s} reply{binding=ZLinkRawServer/Program.cs:74-85,103-109; ladder=ZLinkServer/Program.cs:148-159,205-217; new protobuf body + raw Response header + 2-part submit} costs{source header/body Messages + %s; target one blocking Recv, one parse/metrics update; no payload copy}\n' \
        "$previous" "$new" "$level" "$cell" "$replicate" "$HEAD" "$DIRTY" "$BASE" "$HASHES" "$output" \
        "$( [[ "$level" == L0 ]] && echo 140-159 || echo 196-217 )" \
        "$( [[ "$level" == L0 ]] && echo direct handler || echo permit acquire/release and lease Interlocked.Exchange )" >> "$JOB/progress.md"
      attempt=0
      base="$output"
      while :; do
        export PATH="$TOOLS:$PATH" IMPLEMENTATIONS="$impl" PATTERNS="$pattern" PAYLOAD_SIZES="$size"
        export SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 WARMUP_SECONDS=10 DURATION_SECONDS=5 RUNS=1 OUTPUT_DIR="$base" SCHEDULING_RECORD=1
        export RUN_STAMP="4350-dotnet-${previous}-${new}-${level}-${cell}-r${replicate}-a${attempt}-$(date -u +%Y%m%dT%H%M%SZ)"
        export ZLINK_DOTNET_LADDER_LEVEL="$level" ZLINK_DOTNET_LADDER_CLIENT=raw
        "$RUNNER" > "$JOB/raw/boundary-${previous}-${new}-${level}-${cell}-r${replicate}-a${attempt}.log" 2>&1
        result="$base/run1/${impl}-${pattern}-${size}/results.json"
        if [[ -f "${result}.scheduling-invalid" ]]; then
          printf -- '- invalid scheduling run discarded: %s\n' "$result" >> "$JOB/progress.md"
          attempt=$((attempt + 1))
          base="$JOB/raw/boundary-confirm-retry/${previous}-${new}/${pattern}-${size}/run${replicate}/${level}/attempt${attempt}"
          continue
        fi
        python3 - "$result" "$JOB/progress.md" "$level" "$cell" "$replicate" <<'PY'
import json, pathlib, sys
path, progress, level, cell, run = sys.argv[1:]
c = json.loads(pathlib.Path(path).read_text())['cells'][0]
assert c['errors'] == 0 and c['server_errors'] == 0 and c['target_stats']['errors'] == 0
assert c['submitted'] == c['completed'] == c['target_stats']['received']
schedule = pathlib.Path(path + '.scheduling.txt').read_text()
assert 'phase=process-start' in schedule and 'phase=mid-active' in schedule
assert 'discovery-failed' not in schedule
policies = [line for line in schedule.splitlines() if 'current scheduling policy:' in line]
assert policies and all('current scheduling policy: SCHED_OTHER' in line for line in policies)
with open(progress, 'a') as f:
    f.write(f"- confirmed {level} {cell} run{run}: {c['throughput_per_second']} msg/s; raw={path}; schedule={path}.scheduling.txt\n")
PY
        break
      done
    done
  done
done
record "$JOB/raw/lock-windows/boundary-confirm-after.txt"
