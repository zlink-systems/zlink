#!/bin/bash
set -euo pipefail
export TMPDIR=/root/jobs/fx/cx/v21/s033/cpoll/tmp
if [[ ${CPOLL_MEASURE_LOCKED:-0} != 1 ]]; then
  exec flock -o /root/jobs/fx/cx/measure.lock env CPOLL_MEASURE_LOCKED=1 bash "$0"
fi
job=/root/jobs/fx/cx/v21/s033/cpoll
available=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
(( available >= 12 * 1024 * 1024 )) || { echo "MemAvailable below 12 GiB: $available KiB"; exit 1; }
printf "MemAvailable=%s KiB\n" "$available" > "$job/measure-memory.log"
exec env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$job/build" IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp PATTERNS=request-backpressure,send-saturation PAYLOAD_SIZES=1024 RUNS=3 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP=cpoll-csubmit OUTPUT_DIR="$job/measure" bash "$job/run-B.sh"
