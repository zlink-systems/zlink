#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-s2
LOCKED="$JOB/tools/java/run-step-locked.sh"
flock -x /root/jobs/fx/cx/measure.lock bash -c 'set -euo pipefail; locked="$1"; shift; bash "$locked" L1 repeat request-serial 4096; bash "$locked" L1 repeat request-serial 4096; bash "$locked" L2 repeat request-serial 4096; bash "$locked" L2 repeat request-serial 4096' _ "$LOCKED"
