#!/usr/bin/env bash
set -euo pipefail
BASE=/root/jobs/fx/cx/v21/s033/cbatch
run_one() {
  local name="$1" pattern="$2" impl="$3"
  local log="$BASE/out/$name.console.log"
  mkdir -p "$BASE/out/$name"
  echo "$(date -Iseconds) start $name" | tee -a "$BASE/matrix.log"
  flock -o /root/jobs/fx/cx/measure.lock env CAPTURE_MODE=bpf PATTERNS="$pattern" PAYLOAD_SIZES=1024 IMPLEMENTATIONS="$impl" RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 RUN_STAMP="$name" OUTPUT_DIR="$BASE/out/$name" bash "$BASE/runner/cpp-run_local.sh" >"$log" 2>&1
  echo "$(date -Iseconds) done $name" | tee -a "$BASE/matrix.log"
}
for n in 2 3; do run_one "fw-req-r$n" request-backpressure zlink-framework-cpp; done
for n in 1 2 3; do run_one "fw-send-r$n" send-saturation zlink-framework-cpp; done
for n in 1 2 3; do run_one "bind-req-r$n" request-backpressure zlink-cpp; done
for n in 1 2 3; do run_one "bind-send-r$n" send-saturation zlink-cpp; done
