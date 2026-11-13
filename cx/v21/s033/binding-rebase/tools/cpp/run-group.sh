#!/usr/bin/env bash
set -euo pipefail
label="$1"; runner="$2"; output="$3"; shift 3
JOB=/root/jobs/fx/cx/v21/s033/l3868-m1
mkdir -p "$JOB/raw/lock-windows"
flock -x /root/jobs/fx/cx/measure.lock bash -c '
 set -euo pipefail
 label="$1"; runner="$2"; output="$3"
 JOB=/root/jobs/fx/cx/v21/s033/l3868-m1
 setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
 record(){ { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq; done; } > "$1"; }
 finish(){ record "$JOB/raw/lock-windows/${label}-after.txt"; setgov powersave; }
 trap finish EXIT
 setgov performance
 record "$JOB/raw/lock-windows/${label}-before.txt"
 export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH"
 export WARMUP_SECONDS=3 DURATION_SECONDS=5 RUNS=1 SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15
 export OUTPUT_DIR="$output" RUN_STAMP="m1-${label}" M1_PHASE="$label"
 "$runner" >"$JOB/raw/${label}-driver.log" 2>&1
' _ "$label" "$runner" "$output" "$@"
