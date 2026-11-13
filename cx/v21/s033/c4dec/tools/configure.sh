#!/bin/bash
set -euo pipefail
ulimit -v 26214400
cd /root/worktree/zlink-v21-s033/c4dec
cmake -S framework/languages/cpp -B build-c4dec -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/build-c4dec-deps;/root/.cache/zlink/core/1.25.0/linux-x64" -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=/root/.cache/zlink/core/1.25.0/linux-x64 -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF -DCMAKE_PROJECT_INCLUDE=/root/jobs/fx/cx/v21/s033/c4dec/tools/probes.cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-c4dec -j3 --target c4dec-bulk-baseline c4dec-bulk-candidate-a c4dec-bulk-candidate-b c4dec-parity-candidate-a c4dec-parity-candidate-b c4dec-malloc-counter test_cpp_framework_typed_json_allocations
