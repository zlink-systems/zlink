#!/usr/bin/env bash
set -euo pipefail
JOB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$JOB"
TOOLS="$JOB/tools"
RUNNER="$TOOLS/run-aa-one.sh"
OUTPUT="$JOB/raw/results"
mkdir -p "$JOB/raw/lock-windows" "$JOB/raw/run-env" "$OUTPUT"
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  job="$1"; root="$2"; tools="$3"; runner="$4"; output="$5"
  setgov(){ for cpu in $(seq 0 15); do echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; done; }
  record(){ local path="$1"; { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq"; done; } > "$path"; }
  trap "setgov powersave" EXIT
  setgov performance
  record "$job/raw/lock-windows/aa-noise-before.txt"
  export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH"
  export WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
  export IMPLEMENTATIONS=zlink-java SUBMIT_CONCURRENCY=8 WINDOW=100
  export AA_TOOLS="$tools" AA_ENV_DIR="$job/raw/run-env"
  for cell in request-serial-4096 send-saturation-4096; do
    case "$cell" in request-serial-4096) pattern=request-serial ;; send-saturation-4096) pattern=send-saturation ;; esac
    payload=4096
    for run in $(seq 1 10); do
      for side in A B; do
        stamp="aa4350-${cell}-${side}-run${run}"
        cell_id="zlink-java-${pattern}-${payload}"
        "$tools/progress-before-measurement.sh" "$cell-$side-run$run"
        env AA_CELL="$cell" AA_SIDE="$side" AA_INDEX="$run" AA_TOOLS="$tools" AA_ENV_DIR="$job/raw/run-env" \
          RUNS=1 PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUN_STAMP="$stamp" \
          OUTPUT_DIR="$output/$cell/$side/run$run" "$runner" >"$job/raw/${cell}-${side}-run${run}-driver.log" 2>&1
        result="$output/$cell/$side/run$run/run1/$cell_id/results.json"
        python3 - "$result" <<'PY'
import json,sys
x=json.load(open(sys.argv[1])); c=x["cells"][0]
submitted=int(c.get("submitted",c.get("completed",-1))); completed=int(c["completed"]); received=int(c["target_stats"]["received"]); errors=int(c["errors"]); target_errors=int(c["target_stats"]["errors"])
if errors or target_errors or submitted!=completed or completed!=received:
    raise SystemExit(f"invalid measurement: submitted={submitted} completed={completed} received={received} errors={errors}/{target_errors}")
PY
      done
    done
  done
  record "$job/raw/lock-windows/aa-noise-after.txt"
' _ "$JOB" "$ROOT" "$TOOLS" "$RUNNER" "$OUTPUT"
