#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s1
flock -x /root/jobs/fx/cx/measure.lock "$JOB/tools/node/run-hold.sh" "$@"
