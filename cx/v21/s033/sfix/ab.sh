#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/sfix
for round in 1 2 3 4; do
 for pattern in request-backpressure request-serial send-saturation; do
  for variant in after before; do
   if [[ $variant == after ]]; then tree=/root/worktree/zlink-3867a; else tree=/root/worktree/zlink-ratio040; fi
   (( $(awk '/MemAvailable/{print $2}' /proc/meminfo) >= 12582912 ))
   echo "$(date -Is) round=$round pattern=$pattern variant=$variant"
   env IMPLEMENTATIONS=zlink-framework-cpp PATTERNS="$pattern" PAYLOAD_SIZES=1024 RUNS=1 \
    WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP="sfix-$variant-$round" \
    OUTPUT_DIR="$job/ab/$round/$pattern/$variant" \
    flock -o /root/jobs/fx/cx/measure.lock bash "$tree/framework/bench/grpc/cpp/run_local.sh"
  done
 done
done
