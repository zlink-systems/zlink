#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s2
HOLD=$JOB/tools/node/run-hold-l4-l57.sh
waited=${BUILD_WAIT_SECONDS:-0}
while (( waited < 600 )) && ps -eo stat=,args= | awk '$1 ~ /^[RS]/ && $0 ~ /(cmake|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ { found=1 } END { exit !found }'; do
  sleep 5
  waited=$((waited+5))
done
printf '%s pre-hold build_test_wait_seconds=%s still_active=%s\n' "$(date -u +%FT%TZ)" "$waited" "$(ps -eo stat=,args= | awk '$1 ~ /^[RS]/ && $0 ~ /(cmake|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ {n++} END{print n+0}')" >> "$JOB/progress.md"
while :; do
  if flock -n -E 75 -x /root/jobs/fx/cx/measure.lock "$HOLD"; then exit 0; else status=$?; fi
  if [[ "$status" != 75 ]]; then exit "$status"; fi
  printf '%s lock occupied; waiting outside the lock\n' "$(date -u +%FT%TZ)" >> "$JOB/progress.md"
  sleep 10
done
