#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lfx-cpp
export MEASURE_DEADLINE_EPOCH PYTHONDONTWRITEBYTECODE=1
MEASURE_DEADLINE_EPOCH=$(python3 -c 'import json;print(json.load(open("/root/jobs/fx/cx/v21/s033/lfx-cpp/raw/task-time.json"))["deadline_epoch"])')
remaining=$(( MEASURE_DEADLINE_EPOCH - $(date -u +%s) ))
[[ "$remaining" -gt 0 ]]
phase="${1:-initial}"
rc=0
timeout --signal=TERM --kill-after=10 "$remaining" "$JOB/tools/run-group.sh" "$phase" "$JOB/tools/measure.py" "$JOB/raw" || rc=$?
# The first action after every hold preserves a partial numerical report.
python3 "$JOB/tools/write-report.py" "$phase" --plan
exit "$rc"
