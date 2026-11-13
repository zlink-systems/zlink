#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868-l0f
cd /root/worktree/zlink-v21-s033/l3868
[[ $(cat "$JOB/BUILD_DONE") == rc=0* ]]
export MEASURE_DEADLINE_EPOCH=$(( $(stat -c %Y "$JOB/BUILD_DONE") + 14400 ))
remaining=$(( MEASURE_DEADLINE_EPOCH - $(date -u +%s) ))
[[ "$remaining" -gt 0 ]]
printf 'BUILD_DONE=%s deadline_epoch=%s remaining_seconds=%s\n' "$(stat -c %y "$JOB/BUILD_DONE")" "$MEASURE_DEADLINE_EPOCH" "$remaining" >"$JOB/raw/time-budget.txt"
timeout --signal=TERM --kill-after=10 "$remaining" "$JOB/tools/run-group.sh" receive-parity "$JOB/tools/preflight-measure.sh" "$JOB/raw"
