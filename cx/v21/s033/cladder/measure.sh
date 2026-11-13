#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cladder
rung=${1:?rung};mode=${2:-none};runs=${3:-3};label=${4:-rung$rung}
impl=zlink-cpp
if [[ $rung == 9 ]];then impl=zlink-framework-cpp;fi
exec flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD DEBUGINFOD_URLS= DEBUGINFOD_CACHE_PATH="$job/debuginfod" CAPTURE_MODE="$mode" LADDER_RUNG="$rung" ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$job/build/rung$rung" IMPLEMENTATIONS="$impl" PATTERNS="${LADDER_PATTERNS:-request-backpressure}" PAYLOAD_SIZES=1024 RUNS="$runs" WARMUP_SECONDS=30 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP="cladder-$label" OUTPUT_DIR="$job/raw/$label" bash "$job/run-B.sh"
