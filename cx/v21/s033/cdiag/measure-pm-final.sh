#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cdiag
for variant in main branch; do
 flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD CDIAG_TRACE="$job/pm-final-$variant.csv" PERF_ENABLE=0 PERF_SCHED=1 ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$job/$variant-build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 PM=1 FRAMEWORK_TARGET=bench_cpp_pm_server RUN_STAMP="cdiag-pm-final-$variant" OUTPUT_DIR="$job/pm-final/$variant" bash "$job/run-B.sh"
done
