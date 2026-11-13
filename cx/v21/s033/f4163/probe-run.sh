#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163
kind="$1"
src="$2"
prefix="$JOB/prefix-$kind"
bin="$src/bindings/c/build-release-1.23.0/perf/perf_router_router"
for n in 1 2 3 4 5; do
  out="$JOB/runs/$kind-$n.log"
  [[ ! -e "$out" ]]
  flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 bash -c '
    set -e
    python3 "$1/stage-preflight.py" "$1"
    ldd "$2"
    env PERF_SINGLE_DURATION_SECONDS=5 PERF_PART_COUNT=2 PERF_CTX_AUTO_HWM_PROFILE=balanced PERF_SINGLE_SNDTIMEO_MS=200 PERF_SINGLE_RCVTIMEO_MS=200 PERF_PRINT_AUTO_HWM_DETAIL=1 "$2" c inproc 1024
  ' _ "$JOB" "$bin" > "$out" 2>&1
  tail -2 "$out"
done
