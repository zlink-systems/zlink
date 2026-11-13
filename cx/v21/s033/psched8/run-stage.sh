#!/bin/bash
set -euo pipefail
j=/root/jobs/fx/cx/v21/s033/psched8
python3 "$j/prepare-probes-queue.py"
trap 'perf probe --del "psched8:*" > "$j/stage-probe-cleanup.log" 2>&1' EXIT
flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 \
 BUILD_DIR=/root/worktree/zlink-v21-s033/psched5/.psched8-build IMPLEMENTATIONS=zlink-framework-cpp \
 PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 \
 PM=1 FRAMEWORK_TARGET=bench_cpp_pm_server BENCH_COLLECTOR="$j/stage-collector.py" \
 RUN_STAMP=psched8-stage OUTPUT_DIR="$j/stage/chained" python3 "$j/processes.py" bash "$j/run-B.sh"
