#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/i4162
flock -o /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  taskset -c 0 python3 "$1/observe.py" --runs 2 --label natural-extra
  taskset -c 0 python3 "$1/observe.py" --runs 1 --label queue-cycle --placement separate --aslr-off --sample-cap 1000000 --pad 0 --queue-cycle
' _ "$JOB"
