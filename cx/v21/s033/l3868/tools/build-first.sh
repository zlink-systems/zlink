#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868
cd /root/worktree/zlink-v21-s033/l3868
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build build-l3868 -j3 \
  --target bench_cpp_client bench_cpp_zlink_server bench_cpp_framework_server bench_cpp_grpc_server \
  bench_cpp_raw_wire_test bench_cpp_framework_window_test bench_cpp_framework_completion_test \
  bench_cpp_submit_threads_test bench_cpp_server_metrics_test bench_cpp_grpc_callback_test >"$JOB/raw/build-l0-v4.log" 2>&1 && \
  "$JOB/tools/verify-build.sh" >"$JOB/raw/verify-build.txt" 2>&1 && \
  ZLINK_CPP_LADDER_LEVEL=0 IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp \
    PATTERNS=send-saturation PAYLOAD_SIZES=1024 \
    "$JOB/tools/run-group.sh" first-l0 "$JOB/tools/cpp-runner.sh" "$JOB/raw/L0/send-saturation-1024"
