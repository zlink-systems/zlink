#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lfx-node
HOLD=$JOB/tools/node/run-hold-l4-l57.sh
waited=${BUILD_WAIT_SECONDS:-0}
while (( waited < 600 )) && python3 "$JOB/tools/node/active-builds.py" >/dev/null; do
  sleep 5
  waited=$((waited+5))
done
printf '%s pre-hold build_test_wait_seconds=%s active_builds=%s\n' "$(date -u +%FT%TZ)" "$waited" "$(python3 "$JOB/tools/node/active-builds.py" || true)" >> "$JOB/progress.md"
while :; do
  if flock -n -E 75 -x /root/jobs/fx/cx/measure.lock "$HOLD"; then exit 0; else status=$?; fi
  if [[ "$status" != 75 ]]; then exit "$status"; fi
  printf '%s lock occupied; waiting outside the lock\n' "$(date -u +%FT%TZ)" >> "$JOB/progress.md"
  sleep 10
done
