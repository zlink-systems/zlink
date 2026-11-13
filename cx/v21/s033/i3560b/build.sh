#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-v21-s033/i3560b
cmake -S framework/bench/grpc/cpp -B framework/bench/grpc/cpp/build -G Ninja \
 -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake \
 -DVCPKG_MANIFEST_DIR="$PWD/framework/languages/cpp" -DVCPKG_MANIFEST_FEATURES=bench \
 -DVCPKG_OVERLAY_PORTS="$PWD/vcpkg/ports" -DVCPKG_MAX_CONCURRENCY=3 \
 -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON \
 -Dzlink_cpp_DIR=/root/jobs/fx/cx/v21/s033/i3560b/cmake-shim \
 -DZLINK_CPP_CORE_VERSION=1.23.0 -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=1.23.0 \
 -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24 \
 -DZLINK_CPP_BUILD_TESTS=OFF -DZLINK_CPP_BUILD_EXAMPLES=OFF -DZLINK_CPP_BUILD_PERF=OFF
cmake --build framework/bench/grpc/cpp/build -j3 --target bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server
