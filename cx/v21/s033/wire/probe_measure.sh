#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/wire
for label in main W; do
 mkdir -p "$job/probe-counters-$label"
 flock -o /root/jobs/fx/cx/measure.lock env LD_PRELOAD=/root/jobs/fx/cx/v21/s033/cprof/alloc_counter.so PROF_ALLOC_DIR="$job/probe-counters-$label" "$job/probe-build/alloc-$label" > "$job/probe-$label.json"
done
