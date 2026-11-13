#!/usr/bin/env bash
set -euo pipefail

JOB=/root/jobs/fx/cx/v21/s033/lad-node-l0e
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$WORKTREE/framework/bench/grpc/node/run_local.sh"
RUN_GROUP="$JOB/tools/run-group.sh"

run_pair() {
  local label="$1" output="$2" source_impl="$3" target_impl="$4"
  shift 4
  env PATTERNS=send-saturation,request-serial PAYLOAD_SIZES=1024 RUNS=3 \
    IMPLEMENTATIONS="$source_impl" BENCH_TARGET_SERVER="$target_impl" "$@" \
    bash "$RUN_GROUP" "$label" "$RUNNER" "$output"
}

run_stage() {
  local stage="$1" l0="$2"
  local raw="$JOB/raw/cross/$stage"
  if [[ "$l0" == 1 ]]; then
    run_pair "$stage-raw-raw" "$raw/raw-raw/bench/node" zlink-node zlink-node
    run_pair "$stage-framework-raw" "$raw/framework-raw/bench/node" \
      zlink-node zlink-framework-node \
      ZLINK_CROSS_FRAMEWORK_WIRE=1 ZLINK_LADDER_L0_SERVER=1
    run_pair "$stage-framework-framework" "$raw/framework-framework/bench/node" \
      zlink-framework-node zlink-framework-node \
      ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_FRAMEWORK_CLIENT=1
  else
    run_pair "$stage-raw-raw" "$raw/raw-raw/bench/node" zlink-node zlink-node
    run_pair "$stage-framework-raw" "$raw/framework-raw/bench/node" \
      zlink-node zlink-framework-node ZLINK_CROSS_FRAMEWORK_WIRE=1
    run_pair "$stage-framework-framework" "$raw/framework-framework/bench/node" \
      zlink-framework-node zlink-framework-node
  fi
}

run_stage main 0
run_stage l0 1
