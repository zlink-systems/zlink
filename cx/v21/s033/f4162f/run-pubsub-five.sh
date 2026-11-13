#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/f4162b
JOB=/root/jobs/fx/cx/v21/s033/f4162f
CORE=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24
set_gov() {
  for c in $(seq 0 15); do
    echo "$1" > "/sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor"
  done
}
snapshot() {
  local path="$1"
  {
    date -u +%FT%TZ
    uptime
    free -b
    for c in $(seq 0 15); do
      printf "cpu%s," "$c"
      cat "/sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq"
    done
  } > "$path"
}
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  ROOT="$1"
  JOB="$2"
  CORE="$3"
  set_gov() {
    for c in $(seq 0 15); do
      echo "$1" > "/sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor"
    done
  }
  snapshot() {
    local path="$1"
    {
      date -u +%FT%TZ
      uptime
      free -b
      for c in $(seq 0 15); do
        printf "cpu%s," "$c"
        cat "/sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq"
      done
    } > "$path"
  }
  trap "set_gov powersave" EXIT
  set_gov performance
  for run in 01 02 03 04 05; do
    RESULTS="$JOB/raw/results-pubsub-$run"
    mkdir -p "$RESULTS"
    snapshot "$JOB/raw/final-conditions-$run-before.txt"
    env ZLINK_CORE_SOURCE=release \
        ZLINK_CORE_PACKAGE_PREFIX="$CORE" \
        ZLINK_LIBRARY_PATH="$CORE/lib/libzlink.so.1.23.0" \
        LD_LIBRARY_PATH="$CORE/lib" \
        PERF_ALLOW_MULTI=1 PERF_POLICY=1 PERF_SKIP_AUTO_BUILD=1 \
        PERF_TRANSPORTS=tcp PERF_MSG_SIZES=256 \
        PERF_MULTI_DURATION_SECONDS=5 PERF_MULTI_LATENCY_DURATION_SECONDS=5 \
        PERF_MULTI_CLIENTS=100 PERF_RESULTS_DIR="$RESULTS" \
        python3 "$ROOT/bindings/c/perf/run_comparison.py" PUBSUB --runs 1 --duration 5 \
          --latency-duration 5 --build-dir "$ROOT/.build-4162f" \
          --results-dir "$RESULTS" --multi-transport-transition-ms 0 \
          --multi-pattern-transition-ms 0 > "$JOB/raw/final-pubsub-$run.log" 2>&1
    snapshot "$JOB/raw/final-conditions-$run-after.txt"
  done
' -- "$ROOT" "$JOB" "$CORE"
