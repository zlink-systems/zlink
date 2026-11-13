#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/lad-java
JOB=/root/jobs/fx/cx/v21/s033/lad-java-par
RUNNER="$ROOT/framework/bench/grpc/java/run_local.sh"
mkdir -p "$JOB/raw/lock-windows" "$JOB/raw/fresh/L0-extra"
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  root="$1"; job="$2"; runner="$3"
  setgov() { for cpu in $(seq 0 15); do echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; done; }
  record() {
    local path="$1"
    { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq"; done; } > "$path"
  }
  trap "setgov powersave" EXIT
  setgov performance
  record "$job/raw/lock-windows/remaining-before.txt"
  export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH"
  export WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
  export IMPLEMENTATIONS=zlink-java,zlink-framework-java PATTERNS=request-serial,request-backpressure PAYLOAD_SIZES=1024,4096 RUNS=2
  export ZLINK_BENCH_LADDER_L0=1 OUTPUT_DIR="$job/raw/fresh/L0-extra" RUN_STAMP=4350par-L0-extra
  for run in 1 2; do
    for cell in reqserial-1K reqserial-4K reqbp-1K reqbp-4K; do
      "$job/tools/progress-before-measurement.sh" "L0-$cell-extra-run$run" L0 >/dev/null
    done
  done
  "$runner" >"$job/raw/L0-extra-driver.log" 2>&1
  for level in L1 L2 L3; do
    unset ZLINK_BENCH_LADDER_L0 ZLINK_BENCH_LADDER_L1 ZLINK_BENCH_LADDER_L2 ZLINK_BENCH_LADDER_L3
    case "$level" in
      L1) export ZLINK_BENCH_LADDER_L1=1 ;;
      L2) export ZLINK_BENCH_LADDER_L2=1 ;;
      L3) export ZLINK_BENCH_LADDER_L2=1 ZLINK_BENCH_LADDER_L3=1 ;;
    esac
    export IMPLEMENTATIONS=zlink-java,zlink-framework-java PATTERNS=send-saturation,request-serial,request-backpressure PAYLOAD_SIZES=1024,4096 RUNS=3
    export OUTPUT_DIR="$job/raw/fresh/$level" RUN_STAMP="4350par-$level"
    for run in 1 2 3; do
      for cell in sendsat-1K sendsat-4K reqserial-1K reqserial-4K reqbp-1K reqbp-4K; do
        "$job/tools/progress-before-measurement.sh" "$level-$cell-run$run" "$level" >/dev/null
      done
    done
    "$runner" >"$job/raw/${level}-driver.log" 2>&1
    record "$job/raw/lock-windows/${level}-after.txt"
  done
  record "$job/raw/lock-windows/remaining-after.txt"
' _ "$ROOT" "$JOB" "$RUNNER"
