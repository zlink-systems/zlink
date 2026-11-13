#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cdiag
for pattern in send-saturation request-backpressure; do
 for variant in main branch; do
  build=/root/jobs/fx/cx/v21/s033/cdrain/main-build
  [[ $variant == branch ]] && build=/root/jobs/fx/cx/v21/s033/cdrain2/build
  flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS="$pattern" PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP="cdiag-$variant" OUTPUT_DIR="$job/perf/$pattern/$variant" bash "$job/run-B.sh"
 done
done
