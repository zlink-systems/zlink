#!/usr/bin/env bash
set -euo pipefail
JOB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="/root/worktree/zlink-v21-s033/aa-noise"
TOOLS="$JOB/tools"
RUNNER="$TOOLS/run-aa-one.sh"
OUTPUT="$JOB/raw/results"
mkdir -p "$JOB/raw/lock-windows" "$JOB/raw/run-env" "$OUTPUT"

flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  job="$1"; root="$2"; tools="$3"; runner="$4"; output="$5"
  session_start=$SECONDS
  lock_start_utc="$(date -u +%FT%TZ)"
  setgov(){ for cpu in $(seq 0 15); do echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; done; }
  record(){ local path="$1"; { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq"; done; } > "$path"; }
  trap "setgov powersave" EXIT
  setgov performance
  record "$job/raw/lock-windows/aa-noise5-before.txt"
  export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH"
  export WARMUP_SECONDS=10 DURATION_SECONDS=5 SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
  export IMPLEMENTATIONS=zlink-java SUBMIT_CONCURRENCY=8 WINDOW=100
  export AA_TOOLS="$tools" AA_ENV_DIR="$job/raw/run-env"

  run_once(){
    local cell="$1" pattern="$2" side="$3" run="$4" attempt="$5" suffix="$run"
    [[ "$attempt" -eq 1 ]] || suffix="${run}-retry$((attempt-1))"
    local stamp="aa4350-aa5-${cell}-${side}-run${suffix}"
    local cell_id="zlink-java-${pattern}-4096"
    local env_file="$AA_ENV_DIR/${cell}-${side}-run${suffix}.txt"
    local result="$output/$cell/$side/run${suffix}/run1/$cell_id/results.json"
    if (( SECONDS - session_start >= 870 )); then echo "14m30s lock budget reached before ${cell}-${side}-${suffix}" >&2; return 124; fi
    "$tools/progress-before-measurement.sh" "$cell-$side-run$suffix"
    env AA_CELL="$cell" AA_SIDE="$side" AA_INDEX="$suffix" AA_TOOLS="$tools" AA_ENV_DIR="$AA_ENV_DIR" \
      RUNS=1 PATTERNS="$pattern" PAYLOAD_SIZES=4096 RUN_STAMP="$stamp" \
      OUTPUT_DIR="$output/$cell/$side/run$suffix" "$runner" >"$job/raw/${cell}-${side}-run${suffix}-driver.log" 2>&1
    if grep -q "^scheduling_invalid=" "$env_file"; then
      printf 'attempt_status=invalid_scheduling\n' >> "$env_file"
      echo "invalid scheduling detected: $env_file" >&2
      return 2
    fi
    python3 - "$result" <<'PY'
import json,sys
x=json.load(open(sys.argv[1])); c=x["cells"][0]
submitted=int(c.get("submitted",c.get("completed",-1))); completed=int(c["completed"]); received=int(c["target_stats"]["received"]); errors=int(c["errors"]); target_errors=int(c["target_stats"]["errors"])
if errors or target_errors or submitted!=completed or completed!=received:
    raise SystemExit(f"invalid measurement: submitted={submitted} completed={completed} received={received} errors={errors}/{target_errors}")
PY
    printf 'attempt_status=valid\n' >> "$env_file"
  }

  for cell in request-serial-4096 send-saturation-4096; do
    case "$cell" in request-serial-4096) pattern=request-serial ;; send-saturation-4096) pattern=send-saturation ;; esac
    for run in $(seq 1 5); do
      declare -A state
      for side in A B; do
        if run_once "$cell" "$pattern" "$side" "$run" 1; then state["$side"]=valid
        else status=$?; [[ "$status" -eq 2 ]] || exit "$status"; state["$side"]=invalid; fi
      done
      for side in A B; do
        attempt=2
        while [[ "${state[$side]}" == invalid ]]; do
          if run_once "$cell" "$pattern" "$side" "$run" "$attempt"; then state["$side"]=valid
          else status=$?; [[ "$status" -eq 2 ]] || exit "$status"; fi
          attempt=$((attempt+1))
        done
      done
    done
  done
  record "$job/raw/lock-windows/aa-noise5-after.txt"
' _ "$JOB" "$ROOT" "$TOOLS" "$RUNNER" "$OUTPUT"
