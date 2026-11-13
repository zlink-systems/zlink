#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/crecv2
# All endpoint campaigns are serialized by the same exclusive measurement lock.
flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$job/build" IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp PATTERNS=request-backpressure,send-saturation PAYLOAD_SIZES=1024 RUNS=3 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP=crecv-csubmit OUTPUT_DIR="$job/measure" bash "$job/run-B.sh"
for bounded in bounded unlimited; do
 env_args=()
 label=rung9
 if [[ $bounded == unlimited ]]; then env_args=(LADDER_UNBOUNDED=1); label=rung9-unbounded; fi
 flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD "${env_args[@]}" CAPTURE_MODE=none LADDER_RUNG=9 ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$job/ladder/build/rung9" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUNS=3 WARMUP_SECONDS=30 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP="crecv-$label" OUTPUT_DIR="$job/ladder/raw/$label" bash "$job/ladder/run-B.sh"
done
flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD CAPTURE_MODE=allocstack BUILD_DIR="$job/build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=10 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP=crecv-census OUTPUT_DIR="$job/census/after" bash "$job/census.sh"
python3 "$job/analyze-all.py" > "$job/measurement-summary.log"
