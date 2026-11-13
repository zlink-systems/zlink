#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/wire
for round in 1 2 3 4; do
 for pattern in request-backpressure send-saturation; do
  for label in W main; do
   output="$job/measure/$round-$label-$pattern"
   mkdir -p "$output/alloc"
   echo "$(date -Is) round=$round build=$label pattern=$pattern"
   flock -o /root/jobs/fx/cx/measure.lock env IMPLEMENTATIONS=zlink-framework-cpp PATTERNS="$pattern" PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP="wire-$round-$label" OUTPUT_DIR="$output" PROF_ALLOC_DIR="$output/alloc" bash "$job/run-$label.sh" > "$output/run.log" 2>&1
  done
 done
done
