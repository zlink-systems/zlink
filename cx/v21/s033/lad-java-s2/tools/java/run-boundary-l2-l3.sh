#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-s2
LOCKED="$JOB/tools/java/run-step-locked.sh"
flock -x /root/jobs/fx/cx/measure.lock bash -c 'set -euo pipefail; locked="$1"; shift; for step in L2 L2; do bash "$locked" "$step" repeat request-serial 1024; bash "$locked" "$step" repeat request-backpressure 1024; done; for step in L3 L3; do bash "$locked" "$step" repeat request-serial 1024; bash "$locked" "$step" repeat request-serial 4096; bash "$locked" "$step" repeat request-backpressure 1024; done' _ "$LOCKED"
