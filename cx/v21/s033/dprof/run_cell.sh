#!/usr/bin/env bash
set -euo pipefail
mode="$1" impl="$2" pattern="$3"
job=/root/jobs/fx/cx/v21/s033/dprof
repo=/root/worktree/zlink-ratio040
runner="$repo/framework/bench/grpc/dotnet/run_local.sh"
case "$impl" in
  zlink-dotnet) target_port=5209; source_stats=http://127.0.0.1:5206 ;;
  zlink-framework-dotnet) target_port=5215; source_stats=http://127.0.0.1:5213 ;;
  *) exit 2 ;;
esac
stamp="dprof-$mode-$pattern-$impl"
out="$job/bench-out/$stamp"; raw="$job/raw/$stamp"
mkdir -p "$out" "$raw"
mem_kb() { awk '/MemAvailable:/ {print $2}' /proc/meminfo; }
if (( $(mem_kb) < 12582912 )); then echo "MemAvailable below 12 GiB: $(mem_kb)" | tee "$raw/aborted.txt"; exit 3; fi
pids_for() { lsof -nP -t -iTCP:"$1" -sTCP:LISTEN 2>/dev/null | tail -n 1; }
phase_now() { curl -sSf --max-time 1 "$source_stats/bench/stats" 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin).get("phase",""))' 2>/dev/null || true; }
runner_pid=""; collectors=()
cleanup() {
  if [[ -n "$runner_pid" ]] && kill -0 "$runner_pid" 2>/dev/null; then kill -TERM "$runner_pid" 2>/dev/null || true; wait "$runner_pid" 2>/dev/null || true; fi
  for p in "${collectors[@]}"; do kill -INT "$p" 2>/dev/null || true; done
  for p in "${collectors[@]}"; do wait "$p" 2>/dev/null || true; done
}
trap cleanup EXIT
dur=10; warm=3
if [[ "$mode" == profile ]]; then dur=20; fi
env DOTNET_PerfMapEnabled=1 SKIP_BUILD=1 RUNS=1 WARMUP_SECONDS="$warm" DURATION_SECONDS="$dur" \
  IMPLEMENTATIONS="$impl" PATTERNS="$pattern" PAYLOAD_SIZES=1024 RUN_STAMP="$stamp" OUTPUT_DIR="$out" \
  bash "$runner" >"$raw/runner.log" 2>&1 &
runner_pid=$!
for ((i=0;i<600;i++)); do
  [[ "$(phase_now)" == active ]] && break
  if ! kill -0 "$runner_pid" 2>/dev/null; then wait "$runner_pid" || { echo runner-failed; exit 1; }; echo never-active; exit 1; fi
  sleep .1
done
target_pid="$(pids_for "$target_port")"; source_pid="$(lsof -nP -t -iTCP:"${source_stats##*:}" -sTCP:LISTEN 2>/dev/null | tail -n 1)"
if [[ -z "$target_pid" || -z "$source_pid" || "$(phase_now)" != active ]]; then echo "attach failed target=$target_pid source=$source_pid phase=$(phase_now)" | tee "$raw/attach-failed.txt"; exit 4; fi
printf 'target_pid=%s\nsource_pid=%s\nphase=%s\nMemAvailable_kB=%s\n' "$target_pid" "$source_pid" "$(phase_now)" "$(mem_kb)" > "$raw/pids.txt"
(while kill -0 "$runner_pid" 2>/dev/null; do
  now=$(date +%s.%N)
  for pair in "target:$target_pid" "source:$source_pid"; do
    role=${pair%%:*}; pid=${pair##*:}
    for task in /proc/"$pid"/task/*; do
      [[ -r "$task/stat" ]] || continue
      tid=${task##*/}; comm=$(cat "$task/comm" 2>/dev/null || true)
      ticks=$(python3 - "$task/stat" <<'PYCODE'
import sys
s=open(sys.argv[1]).read(); r=s.rfind(')'); f=s[r+2:].split()
print(int(f[11])+int(f[12]))
PYCODE
      ) 2>/dev/null || continue
      printf '%s,%s,%s,%s,%s,%s\n' "$now" "$role" "$tid" "$ticks" "$(getconf CLK_TCK)" "$comm"
    done
  done
  sleep .25
done) > "$raw/thread-cpu.csv" & name_sampler=$!
if [[ "$mode" == profile ]]; then
  trace=/root/jobs/fx/cx/v21/p032net/tools/dotnet-trace8/dotnet-trace
  counters=/root/jobs/fx/cx/v21/p032net/tools/dotnet-counters
  for role_pid in "target:$target_pid" "source:$source_pid"; do
    role=${role_pid%%:*}; pid=${role_pid##*:}
    "$trace" collect -p "$pid" --profile cpu-sampling --providers 'Microsoft-Windows-DotNETRuntime:0x4000:5' --duration 00:00:00:20 -o "$raw/$role.nettrace" >"$raw/$role.trace.log" 2>&1 & collectors+=("$!")
    "$counters" collect -p "$pid" --counters System.Runtime --refresh-interval 1 --format json --duration 00:00:00:20 -o "$raw/$role.counters.json" >"$raw/$role.counters.log" 2>&1 & collectors+=("$!")
    perf stat -p "$pid" -e task-clock,context-switches,raw_syscalls:sys_enter,syscalls:sys_enter_futex -x, -o "$raw/$role.perf.csv" 2>"$raw/$role.perf.err" & collectors+=("$!")
  done
fi
while kill -0 "$runner_pid" 2>/dev/null; do
  if (( $(mem_kb) < 12582912 )); then echo "MemAvailable fell below 12 GiB: $(mem_kb)" | tee "$raw/aborted.txt"; kill -TERM "$runner_pid" 2>/dev/null || true; exit 3; fi
  sleep 1
done
wait "$runner_pid"; runner_pid=""
wait "$name_sampler" 2>/dev/null || true
for p in "${collectors[@]}"; do wait "$p" 2>/dev/null || true; done
collectors=(); trap - EXIT
echo "complete $mode $pattern $impl $out $raw"
