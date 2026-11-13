#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868-l1
grep -qx 'rc=0' "$JOB/BUILD_DONE"
export MEASURE_DEADLINE_EPOCH
MEASURE_DEADLINE_EPOCH=$(python3 -c 'import json;print(json.load(open("/root/jobs/fx/cx/v21/s033/l3868-l1/raw/task-time.json"))["deadline_epoch"])')
remaining=$(( MEASURE_DEADLINE_EPOCH - $(date -u +%s) ))
[[ "$remaining" -gt 0 ]]
timeout --signal=TERM --kill-after=10 "$remaining" "$JOB/tools/run-group.sh" L0-L1 "$JOB/tools/preflight-measure.sh" "$JOB/raw"
