#!/usr/bin/env bash
set -euo pipefail
label="$1"; runner="$2"
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m1
mkdir -p "$JOB/raw/lock-windows"
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  label="$1"; runner="$2"
  for cpu in $(seq 0 15); do echo performance > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done
  trap '\''for cpu in $(seq 0 15); do echo powersave > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done'\'' EXIT
  { date -u +%FT%TZ; uptime; free -b; } > "/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/lock-windows/${label}-before.txt"
  export PATH="/root/jobs/fx/cx/v21/s033/r4190m/tools:$PATH" SKIP_BUILD=1
  "$runner"
  { date -u +%FT%TZ; uptime; free -b; } > "/root/jobs/fx/cx/v21/s033/lad-node-m1/raw/lock-windows/${label}-after.txt"
' _ "$label" "$runner" >"$JOB/raw/${label}-driver.log" 2>&1
