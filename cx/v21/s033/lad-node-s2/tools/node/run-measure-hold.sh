#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s2
TOOLS=$JOB/tools/node
# Builds must have a chance to finish before this exclusive measurement window.
waited=0
while ps -eo stat=,args= | awk '$1 ~ /^[RS]/ && $0 ~ /(cmake|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ { found=1 } END { exit !found }'; do
  if (( waited >= 600 )); then echo 'build remained active for 10 minutes; no measurement started' >&2; exit 124; fi
  sleep 5
  waited=$((waited+5))
done
printf '%s pre-hold build_wait_seconds=%s\n' "$(date -u +%FT%TZ)" "$waited" >> "$JOB/progress.md"
flock -x /root/jobs/fx/cx/measure.lock "$TOOLS/run-hold-l4-l57.sh"
