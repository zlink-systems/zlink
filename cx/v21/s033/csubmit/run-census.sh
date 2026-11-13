#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/csubmit
for variant in main branch; do
 build="$job/main-build"; [[ $variant == branch ]] && build="$job/build"
 flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD CAPTURE_MODE=allocstack BUILD_DIR="$build" \
  IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 \
  RUNS=1 WARMUP_SECONDS=10 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP="csubmit-census-$variant" \
  OUTPUT_DIR="$job/census/$variant" bash "$job/census.sh"
done
python3 "$job/analyze-census.py"
