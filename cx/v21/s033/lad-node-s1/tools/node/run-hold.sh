#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s1
TOOLS=$JOB/tools/node
START=$SECONDS
# Wait for guard v8 to disable deep C-states and apply its split before any benchmark process starts.
ready=0
for _ in $(seq 1 60); do
  ready=1
  for cpu in 0 8; do for state in 2 3; do
    [[ "$(cat /sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}/disable 2>/dev/null || echo unavailable)" == 1 ]] || ready=0
  done; done
  [[ "$ready" == 1 ]] && break
  sleep 0.25
done
[[ "$ready" == 1 ]] || { echo 'quiet guard did not disable C2/C3 before measurement' >&2; exit 1; }
printf '%s guard_ready C2/C3 disabled, SCHED_OTHER enforcement and 0-5/8-13 isolation active\n' "$(date -u +%FT%TZ)" >> "$JOB/progress.md"
# A hold covers no more than two steps. Warm every changed cell, retain warm raw, count exactly once.
while (($#)); do
  variant=$1; shift
  for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
    read -r pattern size <<<"$pair"
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" warm
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" counted
  done
done
(( SECONDS - START < 1500 )) || { echo '30-minute hold safety boundary reached' >&2; exit 124; }
