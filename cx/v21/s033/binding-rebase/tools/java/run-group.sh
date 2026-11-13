#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-m1
mkdir -p "$JOB/raw/lock-windows"
flock -x /root/jobs/fx/cx/measure.lock "$JOB/tools/run-group-locked.sh" >>"$JOB/raw/session2-driver.log" 2>&1
