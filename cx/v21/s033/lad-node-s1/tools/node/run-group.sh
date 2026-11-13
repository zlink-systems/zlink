#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s1
exec 9>/root/jobs/fx/cx/measure.lock
if ! flock -n -x 9; then
  echo 'measure.lock is occupied; no measurement hold was queued' >&2
  exit 75
fi
"$JOB/tools/node/run-hold.sh" "$@"
