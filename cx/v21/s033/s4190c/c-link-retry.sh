#!/bin/bash
set -euo pipefail
mkdir -p framework/bench/grpc/c/build/bench-deps
ln -sfn /root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed/x64-linux/lib/libutf8_range.a framework/bench/grpc/c/build/bench-deps/libutf8_range_lib.a
export LIBRARY_PATH=/root/worktree/zlink-v21-s033/s4190c/framework/bench/grpc/c/build/bench-deps${LIBRARY_PATH:+:${LIBRARY_PATH}}
exec cmake --build framework/bench/grpc/c/build -j3 --target bench_c_with_grpc_grpc_client bench_c_with_grpc_grpc_server
