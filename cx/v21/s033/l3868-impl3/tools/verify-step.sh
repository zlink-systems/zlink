#!/usr/bin/env bash
set -euo pipefail
set -o noclobber
JOB=/root/jobs/fx/cx/v21/s033/l3868-impl3
STAGE=$1
LEVEL=$2
cd /root/worktree/zlink-v21-s033/l3868-next
python3 "$JOB/tools/check-freshness.py" "$JOB/raw/freshness-$STAGE.json" \
  >"$JOB/raw/freshness-$STAGE.log" 2>&1
ZLINK_CPP_LADDER_TEST_LOG_PREFIX="$JOB/raw/contract-$STAGE" \
  ctest --test-dir build-l3868-next -R '^bench_cpp_' --output-on-failure -j1 \
  >"$JOB/raw/ctest-$STAGE.log" 2>&1
cp build-l3868-next/Testing/Temporary/LastTest.log "$JOB/raw/ctest-$STAGE-detail.log"
python3 "$JOB/tools/dry-server.py" "$LEVEL" "$STAGE" \
  >"$JOB/raw/dry-$STAGE-output.log" 2>&1
cp build-l3868-next/compile_commands.json "$JOB/raw/compile-commands-$STAGE.json"
cp build-l3868-next/CMakeCache.txt "$JOB/raw/CMakeCache-$STAGE.txt"
ldd build-l3868-next/bench_cpp_framework_server >"$JOB/raw/ldd-$STAGE.log"
git diff --check >"$JOB/raw/diff-check-$STAGE.log" 2>&1
