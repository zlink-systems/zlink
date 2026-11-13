#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-s2
flock -x /root/jobs/fx/cx/measure.lock "$JOB/tools/java/run-step-locked.sh" "$@"
