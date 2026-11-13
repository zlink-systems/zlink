#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s1
if flock -n -x /root/jobs/fx/cx/measure.lock "$JOB/tools/node/run-hold.sh" "$@"; then
  exit 0
else
  echo 'measure.lock is occupied; no measurement hold was queued' >&2
  exit 75
fi
