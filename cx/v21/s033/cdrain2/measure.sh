#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cdrain2
for round in 1 2 3; do
 for pattern in request-backpressure send-saturation; do
  for variant in main branch; do
   build="/root/jobs/fx/cx/v21/s033/cdrain/main-build"; [[ $variant == branch ]] && build="$job/build"
   flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$build" IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp \
    PATTERNS="$pattern" PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 \
    RUN_STAMP="cdrain-$variant-$round" OUTPUT_DIR="$job/measure/$round/$pattern/$variant" bash "$job/run-B.sh"
  done
 done
 for variant in main branch; do
  build="/root/jobs/fx/cx/v21/s033/cdrain/main-build"; [[ $variant == branch ]] && build="$job/build"
  flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp \
   PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 PM=1 \
   FRAMEWORK_TARGET=bench_cpp_pm_server RUN_STAMP="cdrain-pm-$variant-$round" OUTPUT_DIR="$job/measure/$round/pm/$variant" bash "$job/run-B.sh"
 done
done
