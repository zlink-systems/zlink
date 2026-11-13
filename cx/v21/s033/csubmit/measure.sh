#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/csubmit
for round in 1 2 3; do
 for pattern in request-backpressure send-saturation; do
  for variant in main branch; do
   build="$job/main-build"; [[ $variant == branch ]] && build="$job/build"
   flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$build" IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp \
    PATTERNS="$pattern" PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 \
    RUN_STAMP="cdrain-$variant-$round" OUTPUT_DIR="$job/measure/$round/$pattern/$variant" bash "$job/run-B.sh"
  done
 done
done
