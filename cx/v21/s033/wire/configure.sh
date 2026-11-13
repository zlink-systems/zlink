#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-3867w
cmake -S framework/languages/cpp -B /root/jobs/fx/cx/v21/s033/wire/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_INSTALLED_DIR=/root/worktree/zlink-ratio040/framework/bench/grpc/cpp/build/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT=/root/worktree/zlink-ratio040/.artifacts/wsl -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF
cmake --build /root/jobs/fx/cx/v21/s033/wire/build --parallel 3 --target test_cpp_framework_service_wire_codec -- -k 0
