#!/usr/bin/env bash
set -euo pipefail
label="$1"; runner="$2"; output="$3"; shift 3
JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet
TOOLS=/root/jobs/fx/cx/v21/s033/r4190m/tools
mkdir -p "$JOB/raw/lock-windows"
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  label="$1"; runner="$2"; output="$3"; shift 3
  setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
  record(){ local path="$1"; { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq; done; } > "$path"; }
  trap "setgov powersave" EXIT
  setgov performance
  record "/root/jobs/fx/cx/v21/s033/lad-dotnet/raw/lock-windows/${label}-before.txt"
  export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH"
  export WARMUP_SECONDS=30 DURATION_SECONDS=5 RUNS=3 SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
  export OUTPUT_DIR="$output" RUN_STAMP="4190w-${label}"
  "$runner" >"/root/jobs/fx/cx/v21/s033/lad-dotnet/raw/${label}-driver.log" 2>&1
  record "/root/jobs/fx/cx/v21/s033/lad-dotnet/raw/lock-windows/${label}-after.txt"
' _ "$label" "$runner" "$output" "$@"
