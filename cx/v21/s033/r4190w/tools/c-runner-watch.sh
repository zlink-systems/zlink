#!/usr/bin/env bash
set -euo pipefail
RUNNER=/root/worktree/zlink-v21-s033/r4190m/framework/bench/grpc/c/run_local.sh
CAPTURE=/root/jobs/fx/cx/v21/s033/r4190m/tools/proc_capture_c.py
export BUILD_DIR=/root/jobs/fx/cx/v21/s033/r4190m/tools/c-bin
"$RUNNER" &
run_pid=$!
python3 "$CAPTURE" "$OUTPUT_DIR" "$run_pid" &
watch_pid=$!
status=0
wait "$run_pid" || status=$?
wait "$watch_pid" || status=$?
exit "$status"
