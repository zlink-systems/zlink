#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/salloc
cmake --build "$job/build" --target test_cpp_framework_send_allocations --parallel 3 -- -k 0
cmake --build "$job/build" --parallel 3 -- -k 0
cmake -S /root/worktree/zlink-3867a/framework/languages/cpp -B "$job/debug-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR=/root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT=/root/worktree/zlink-ratio040/.artifacts/wsl \
  -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF \
  -DZLINK_FRAMEWORK_CPP_TEST_PROBES=ON \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
cmake --build "$job/debug-build" --target test_cpp_framework_send_allocations --parallel 3 -- -k 0
