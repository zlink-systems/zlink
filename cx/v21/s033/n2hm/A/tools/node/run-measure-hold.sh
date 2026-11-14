#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/n2hm/A
TOOLS=$JOB/tools/node
waited=${BUILD_WAIT_SECONDS:-0}
while (( waited < 600 )) && python3 "$TOOLS/active-builds.py" >/dev/null; do
  sleep 5
  waited=$((waited+5))
done
active=0
if python3 "$TOOLS/active-builds.py" >/dev/null; then :; else active=1; fi
printf '%s pre-hold build_wait_seconds=%s active_builds_remain=%s\n' "$(date -u +%FT%TZ)" "$waited" "$active" >> "$JOB/progress.md"
flock -x /root/jobs/fx/cx/measure.lock "$TOOLS/run-hold.sh" "$@"
