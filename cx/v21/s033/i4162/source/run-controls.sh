#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/i4162
flock -o /root/jobs/fx/cx/measure.lock bash -c '
  set -euo pipefail
  taskset -c 0 python3 "$1/observe.py" --runs 1 --label cap-A --placement separate --aslr-off --sample-cap 1000000 --pad 0
  taskset -c 0 python3 "$1/observe.py" --runs 1 --label cap-B --placement separate --aslr-off --sample-cap 0 --pad 6
  taskset -c 0 python3 "$1/observe.py" --runs 1 --label cap-A2 --placement separate --aslr-off --sample-cap 1000000 --pad 0
' _ "$JOB"
