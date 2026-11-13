#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/psmeas
src=/root/worktree/zlink-v21-s033/psched5
exec /root/jobs/fx/cx/cpp-build-slot.sh cmake \
 -S "$src/framework/languages/cpp" -B "$src/.psched7-build" -G Ninja \
 -DCMAKE_PROJECT_zlink_framework_cpp_INCLUDE="$job/bench/defer.cmake" \
 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG' \
 -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake \
 -DVCPKG_MANIFEST_MODE=OFF \
 -DVCPKG_INSTALLED_DIR=/root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed \
 -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64 \
 -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX=/root/.cache/zlink/zlink-cpp/1.23.0 \
 -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
 -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=ON -DZLINK_FRAMEWORK_CPP_TEST_PROBES=OFF
