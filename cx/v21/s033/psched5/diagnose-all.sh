#!/bin/bash
set -euo pipefail
j=/root/jobs/fx/cx/v21/s033/psched5
python3 "$j/prepare-probes-all.py"
trap 'perf probe --del "psched5:*" > "$j/probe-cleanup.log" 2>&1' EXIT
export BUILD_DIR=/root/worktree/zlink-v21-s033/psmeas-branch/.psmeas-build
export IMPLEMENTATIONS=zlink-framework-cpp PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 SKIP_BUILD=1
export ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0
export BENCH_COLLECTOR="$j/collect-all.py"
env PM=1 FRAMEWORK_TARGET=bench_cpp_pm_server PATTERNS=send-saturation DURATION_SECONDS=30 OUTPUT_DIR="$j/diagnosis/pm-all" python3 "$j/processes.py" bash "$j/run-B.sh"
