#!/usr/bin/env bash
set -euo pipefail
label="$1"; shift
JOB=/root/jobs/fx/cx/v21/s033/r4190m
mkdir -p "$JOB/raw/lock-windows"
flock -x /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  set_gov() { for cpu in $(seq 0 15); do echo "$1" > "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; done; }
  record() {
    local path="$1"; { date -u +%FT%TZ; uptime; free -b; for cpu in $(seq 0 15); do printf "cpu%s.governor=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor"; printf "cpu%s.scaling_cur_freq_khz=" "$cpu"; cat "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_cur_freq"; done; } > "$path"
  }
  trap "set_gov powersave" EXIT
  set_gov performance
  record "$1"
  shift
  after="${!#}"
  command=("${@:1:$#-1}")
  "${command[@]}"
  record "$after"
' _ "$JOB/raw/lock-windows/$label-before.txt" "$@" "$JOB/raw/lock-windows/$label-after.txt"
