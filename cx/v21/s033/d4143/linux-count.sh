#!/bin/bash
P=/root/.cache/zlink/core-release/1.24.0/linux-x64 J=/root/jobs/fx/cx/v21/s033/d4143 B=/root/worktree/zlink-v21-s033/d4143/build-rel
cd /root/worktree/zlink-v21-s033/d4143/framework/languages/cpp || exit 1
[ -f $B/CMakeCache.txt ] || cmake --preset linux-ninja-release -B $B -DCMAKE_TOOLCHAIN_FILE=/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=$P -Dzlink_DIR=$P/lib/cmake/zlink -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF > $J/configure2.log 2>&1 || { echo configure failed; exit 1; }
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build $B -j3 --target test_cpp_framework_request_call_allocations > $J/build2.log 2>&1 || { echo build failed; tail -5 $J/build2.log; exit 1; }
t=$(find $B -name test_cpp_framework_request_call_allocations -type f -executable | head -1); echo "bin $t"
LD_LIBRARY_PATH=$P/lib $t > $J/linux-run.log 2>&1; echo "test rc=$?"
grep -E "total=|FAILED|PASSED" $J/linux-run.log
