#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/crecv3
flock -o /root/jobs/fx/cx/measure.lock bash -c '
 set -euo pipefail
 job=/root/jobs/fx/cx/v21/s033/crecv3
 available=$(awk '\''/MemAvailable/{print $2}'\'' /proc/meminfo)
 (( available >= 12 * 1024 * 1024 )) || { echo "MemAvailable below 12 GiB: $available KiB"; exit 1; }
 printf "MemAvailable=%s KiB\n" "$available" > "$job/measure-memory.log"
 env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$job/build" IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp PATTERNS=request-backpressure,send-saturation PAYLOAD_SIZES=1024 RUNS=3 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP=crecv3-csubmit OUTPUT_DIR="$job/measure" bash "$job/run-B.sh"
 env -u LD_PRELOAD CAPTURE_MODE=allocstack BUILD_DIR="$job/build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=10 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP=crecv3-census OUTPUT_DIR="$job/census/after" bash "$job/census.sh"
'
