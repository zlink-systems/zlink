#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cthin
mode=${1:-profile}
output=${2:-$mode}
impls=${3:-zlink-cpp,zlink-framework-cpp}
exec flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD CAPTURE_MODE="$mode" ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR=/root/jobs/fx/cx/v21/s033/csubmit/build IMPLEMENTATIONS="$impls" PATTERNS=request-backpressure,send-saturation PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP="cthin-$mode" OUTPUT_DIR="$job/$output" bash "$job/run-B.sh"
