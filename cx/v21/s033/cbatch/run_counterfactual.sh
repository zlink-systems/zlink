#!/usr/bin/env bash
set -euo pipefail
BASE=/root/jobs/fx/cx/v21/s033/cbatch
run_one() {
  local name="$1" pattern="$2" rate="$3"
  local log="$BASE/out/$name.console.log"
  mkdir -p "$BASE/out/$name"
  echo "$(date -Iseconds) start $name rate=$rate" | tee -a "$BASE/matrix.log"
  flock -o /root/jobs/fx/cx/measure.lock env CAPTURE_MODE=bpf ZLINK_RATE_LIMIT="$rate" PATTERNS="$pattern" PAYLOAD_SIZES=1024 IMPLEMENTATIONS=zlink-cpp RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 RUN_STAMP="$name" OUTPUT_DIR="$BASE/out/$name" bash "$BASE/runner/cpp-run_local.sh" >"$log" 2>&1
  echo "$(date -Iseconds) done $name" | tee -a "$BASE/matrix.log"
}
for n in 1 2 3; do run_one "paced-req-r$n" request-backpressure 49947; done
for n in 1 2 3; do run_one "paced-send-r$n" send-saturation 180002; done
