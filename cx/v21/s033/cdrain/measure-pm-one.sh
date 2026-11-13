#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cdrain
for variant in main branch; do
 build="$job/main-build"; [[ $variant == branch ]] && build="$job/build"
 flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp \
 PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 PM=1 \
 FRAMEWORK_TARGET=bench_cpp_pm_server RUN_STAMP="cdrain-pm-$variant-1" OUTPUT_DIR="$job/measure/1/pm/$variant" bash "$job/run-B.sh"
done
