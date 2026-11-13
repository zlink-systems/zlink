#!/bin/bash
set -euo pipefail
j=/root/jobs/fx/cx/v21/s033/bstage
base=/root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build
common=(env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$base" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=20 SKIP_BUILD=1 TARGET_RATE=48000)
run_a() {
  local label=$1
  flock -o /root/jobs/fx/cx/measure.lock "${common[@]}" BENCH_COLLECTOR="$j/collect-22k-A.py" RUN_STAMP="bstage-$label" OUTPUT_DIR="$j/measure/$label/runner" bash /root/jobs/fx/cx/v21/s033/psmeas/run-B.sh >"$j/$label.log" 2>&1
}
run_b() {
  local label=$1
  flock -o /root/jobs/fx/cx/measure.lock "${common[@]}" BENCH_COLLECTOR="$j/collect-22k-B.py" RUN_STAMP="bstage-$label" OUTPUT_DIR="$j/measure/$label/runner" python3 /root/jobs/fx/cx/v21/s033/psched8/processes.py bash /root/jobs/fx/cx/v21/s033/psched8/run-B.sh >"$j/$label.log" 2>&1
}
run_a A1
run_b B1
run_a A2
run_b B2
