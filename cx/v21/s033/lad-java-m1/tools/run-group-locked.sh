#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/lad-java
JOB=/root/jobs/fx/cx/v21/s033/lad-java-m1
RUNNER="$ROOT/framework/bench/grpc/java/run_local.sh"
PROGRESS="$JOB/tools/progress-before-measurement.sh"
setgov() { for cpu in $(seq 0 15); do echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; done; }
record() {
  local path="$1"
  { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq"; done; } > "$path"
}
trap 'setgov powersave' EXIT
setgov performance
record "$JOB/raw/lock-windows/session3-before.txt"
export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH"
export SKIP_BUILD=1 WARMUP_SECONDS=10 DURATION_SECONDS=5 RUNS=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
export IMPLEMENTATIONS=zlink-framework-java PATTERNS=send-saturation,request-serial,request-backpressure PAYLOAD_SIZES=1024,4096
for level in L0 L1 L2 L3; do
  unset ZLINK_BENCH_LADDER_L0 ZLINK_BENCH_LADDER_L1 ZLINK_BENCH_LADDER_L2 ZLINK_BENCH_LADDER_L3
  case "$level" in
    L0) export ZLINK_BENCH_LADDER_L0=1 ;;
    L1) export ZLINK_BENCH_LADDER_L1=1 ;;
    L2) export ZLINK_BENCH_LADDER_L2=1 ;;
    L3) export ZLINK_BENCH_LADDER_L2=1 ZLINK_BENCH_LADDER_L3=1 ;;
  esac
  for cell in sendsat-1K sendsat-4K reqserial-1K reqserial-4K reqbp-1K reqbp-4K; do
    "$PROGRESS" "$level-$cell" "$level" >/dev/null
  done
  export OUTPUT_DIR="$JOB/raw/session3/$level" RUN_STAMP="4351-4353-m1-session3-$level"
  "$RUNNER" >"$JOB/raw/session3-${level}-driver.log" 2>&1 &
  runner_pid=$!
  first_result="$JOB/raw/session3/$level/run1/zlink-framework-java-send-saturation-1024/results.json"
  first_logged=0
  while kill -0 "$runner_pid" 2>/dev/null && [[ "$first_logged" == 0 ]]; do
    if [[ -f "$first_result" ]] && python3 - "$first_result" <<'PY'
import json, pathlib, sys
c = json.loads(pathlib.Path(sys.argv[1]).read_text())['cells'][0]
raise SystemExit(0 if c.get('target_stats') is not None else 1)
PY
    then
      python3 - "$JOB/progress.md" "$first_result" "$level" <<'PY'
import json, pathlib, sys
progress, raw, level = sys.argv[1:]
c = json.loads(pathlib.Path(raw).read_text())['cells'][0]
with open(progress, 'a') as f:
    f.write(f"\nFirst result {level} send-saturation-1K: {c['throughput_per_second']} msg/s; errors={c['errors']}; submitted={c['submitted']} completed={c['completed']} received={c['target_stats']['received']}; raw={raw}.\n")
PY
      first_logged=1
    else
      sleep 0.5
    fi
  done
  wait "$runner_pid"
  for cell in sendsat-1K sendsat-4K reqserial-1K reqserial-4K reqbp-1K reqbp-4K; do
    case "$cell" in
      sendsat-1K) name=send-saturation; size=1024 ;;
      sendsat-4K) name=send-saturation; size=4096 ;;
      reqserial-1K) name=request-serial; size=1024 ;;
      reqserial-4K) name=request-serial; size=4096 ;;
      reqbp-1K) name=request-backpressure; size=1024 ;;
      reqbp-4K) name=request-backpressure; size=4096 ;;
    esac
    path="$JOB/raw/session3/$level/run1/zlink-framework-java-$name-$size/results.json"
    python3 - "$path" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
c = json.loads(p.read_text())['cells'][0]
if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted'] != c['completed'] or c['submitted'] != c['target_stats']['received']:
    raise SystemExit(f'broken result: {p}')
PY
  done
  record "$JOB/raw/lock-windows/session3-$level-after.txt"
done
record "$JOB/raw/lock-windows/session3-after.txt"
