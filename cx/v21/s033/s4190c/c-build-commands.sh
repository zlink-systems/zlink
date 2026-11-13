#!/bin/bash
set -euo pipefail
BENCH_DEPS=/root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed/x64-linux
export PKG_CONFIG_PATH="${BENCH_DEPS}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
cmake -S framework/bench/grpc/c -B framework/bench/grpc/c/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPKG_CONFIG_ARGN=--static -DWG_PROTOC="${BENCH_DEPS}/tools/protobuf/protoc-33.4.0" -DWG_GRPC_CPP_PLUGIN="${BENCH_DEPS}/tools/grpc/grpc_cpp_plugin" -DZLINK_C_CORE_BUILD_DIR=/root/.cache/zlink/core-release/1.24.0/linux-x64 > /root/jobs/fx/cx/v21/s033/s4190c/configure-c.log 2>&1
mkdir -p framework/bench/grpc/c/build/bench-deps
ln -sfn "${BENCH_DEPS}/lib/libutf8_range.a" framework/bench/grpc/c/build/bench-deps/libutf8_range_lib.a
export LIBRARY_PATH=/root/worktree/zlink-v21-s033/s4190c/framework/bench/grpc/c/build/bench-deps${LIBRARY_PATH:+:${LIBRARY_PATH}}
cmake --build framework/bench/grpc/c/build -j3 --target bench_c_with_grpc_zlink_client bench_c_with_grpc_zlink_server bench_c_with_grpc_grpc_client bench_c_with_grpc_grpc_server bench_c_send_metrics_test bench_c_raw_wire_client_test bench_c_raw_wire_server_test > /root/jobs/fx/cx/v21/s033/s4190c/build-c.log 2>&1
