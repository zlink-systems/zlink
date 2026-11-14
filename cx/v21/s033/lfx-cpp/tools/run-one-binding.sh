#!/usr/bin/env bash
set -euo pipefail
LANG_DIR="/root/jobs/fx/cx/v21/s033/lfx-cpp/tools"
BENCH_DIR="/root/worktree/zlink-v21-s033/cfx/framework/bench/grpc/cpp"
impl="${IMPL:-zlink-cpp}"
pattern="$1"; payload="$2"; out="$3"; stamp="$4"; warmup="$5"
mkdir -p "$out"
export SKIP_BUILD=1 RUNS=1 IMPLEMENTATIONS="$impl" PATTERNS="$pattern" PAYLOAD_SIZES="$payload"
export WARMUP_SECONDS="$warmup" DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13
export OUTPUT_DIR="$out" RUN_STAMP="$stamp" SUBMIT_CONCURRENCY=8 WINDOW=100 SCHEDULING_RECORD=1
bash "$LANG_DIR/run_local_binding.sh" >"$out/driver.log" 2>&1
