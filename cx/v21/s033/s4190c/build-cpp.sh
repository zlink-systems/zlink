#!/bin/bash
set -euo pipefail
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64
exec /root/jobs/fx/cx/cpp-build-slot.sh cmake --build framework/bench/grpc/cpp/build -j3 --target bench_cpp_client bench_cpp_grpc_server bench_cpp_zlink_server bench_cpp_framework_server bench_cpp_server_metrics_test bench_cpp_raw_wire_test bench_cpp_grpc_callback_test bench_cpp_framework_window_test bench_cpp_framework_completion_test bench_cpp_submit_threads_test
