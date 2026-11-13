#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4162
WORK=/root/worktree/zlink-v21-s033/f4162
RUNNER="$WORK/bindings/c/perf/run_comparison.py"
run_one() {
  local variant="$1" pattern="$2" transport="$3" size="$4" run="$5"
  local bin_dir="$JOB/bin-$variant" stem="$JOB/raw/multi-${pattern}-${variant}-${run}"
  local results="$JOB/raw/results-${pattern}-${variant}-${run}"
  env ZLINK_CORE_SOURCE=release \
      ZLINK_CORE_PACKAGE_PREFIX=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24 \
      ZLINK_LIBRARY_PATH=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0 \
      LD_LIBRARY_PATH=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib \
      PERF_ALLOW_MULTI=1 PERF_POLICY=1 PERF_SKIP_AUTO_BUILD=1 \
      PERF_TRANSPORTS="$transport" PERF_MSG_SIZES="$size" \
      PERF_MULTI_DURATION_SECONDS=5 PERF_MULTI_LATENCY_DURATION_SECONDS=5 \
      PERF_MULTI_CLIENTS=100 PERF_RESULTS_DIR="$results" \
      python3 "$RUNNER" "$pattern" --runs 1 --duration 5 \
        --latency-duration 5 --build-dir "$bin_dir" --results-dir "$results" \
        --multi-transport-transition-ms 0 --multi-pattern-transition-ms 0 \
        >"$stem.log" 2>&1
}
for run in 1 2 3; do
  run_one before PUBSUB tcp 256 "$run"
  run_one new PUBSUB tcp 256 "$run"
  run_one before DEALER_DEALER tls 64 "$run"
  run_one new DEALER_DEALER tls 64 "$run"
done
