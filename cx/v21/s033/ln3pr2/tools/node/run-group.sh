#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/ln3pr
TOOLS="$JOB/tools/node"
if (( $# == 0 )); then echo 'supply one or more pattern-size cells' >&2; exit 2; fi
running=$(ps -eo stat=,comm=,args= | awk '$1 ~ /^[RS]/ && $2 != "awk" && $0 ~ /(cmake|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ {print}')
if [[ -n "$running" ]]; then
  printf '%s\n' "$(date -u +%FT%TZ) running build found before hold; waiting up to 10 minutes" "$running" >> "$JOB/progress.md"
  deadline=$((SECONDS+600))
  while [[ -n "$running" && $SECONDS -lt $deadline ]]; do sleep 5; running=$(ps -eo stat=,comm=,args= | awk '$1 ~ /^[RS]/ && $2 != "awk" && $0 ~ /(cmake|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ {print}') ; done
  if [[ -z "$running" ]]; then
    printf '%s\n' "$(date -u +%FT%TZ) build wait finished" >> "$JOB/progress.md"
  else
    printf '%s\n' "$(date -u +%FT%TZ) 10-minute build wait elapsed; attempting exclusive hold now" >> "$JOB/progress.md"
  fi
else
  printf '%s no matching builds in R/S state before hold; no wait required\n' "$(date -u +%FT%TZ)" >> "$JOB/progress.md"
fi
if flock -n -E 75 -x /root/jobs/fx/cx/measure.lock "$TOOLS/run-hold.sh" "$@"; then
  printf '%s hold complete cells=%s\n' "$(date -u +%FT%TZ)" "$*" >> "$JOB/progress.md"
else
  rc=$?
  if [[ "$rc" == 75 ]]; then echo 'measure.lock is occupied; no measurement hold was queued' >&2; fi
  exit "$rc"
fi
