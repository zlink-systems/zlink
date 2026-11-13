#!/usr/bin/env bash
set -euo pipefail
ulimit -v 26214400
JOB=/root/jobs/fx/cx/v21/s033/l3868-impl2
cd /root/worktree/zlink-v21-s033/l3868-next
cmake -S framework/bench/grpc/cpp -B build-l3868-next -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_DIR=/root/worktree/zlink-v21-s033/l3868-next/framework/languages/cpp \
  -DVCPKG_MANIFEST_FEATURES=bench \
  -DVCPKG_INSTALLED_DIR=/root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed \
  -DVCPKG_OVERLAY_PORTS=/root/worktree/zlink-v21-s033/l3868-next/vcpkg/ports \
  -DZLINK_CPP_ABLATION_LADDER=ON \
  -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON \
  -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=/root/.cache/zlink/core-release/1.25.0/linux-x64 \
  >"$JOB/raw/configure.log" 2>&1 &&
cmake --build build-l3868-next -j3 \
  --target bench_cpp_client bench_cpp_zlink_server bench_cpp_framework_server bench_cpp_grpc_server \
  bench_cpp_raw_wire_test bench_cpp_framework_window_test bench_cpp_framework_completion_test \
  bench_cpp_submit_threads_test bench_cpp_server_metrics_test bench_cpp_grpc_callback_test bench_cpp_ladder_contract_test \
  >"$JOB/raw/build.log" 2>&1
