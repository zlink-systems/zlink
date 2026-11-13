#!/usr/bin/env bash
set -euo pipefail
flock -x /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/v21/s033/lad-java-s1/tools/java/run-group-locked.sh
python3 /root/jobs/fx/cx/v21/s033/lad-java-s1/tools/java/aggregate.py
