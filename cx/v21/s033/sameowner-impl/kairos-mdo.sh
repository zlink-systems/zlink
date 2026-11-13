#!/bin/bash
J=/root/jobs/fx/cx/v21/s033/sameowner-impl B=/root/worktree/zlink-v21-s033/sameowner-impl/build-sameowner
cd /root/worktree/zlink-v21-s033/sameowner-impl/framework/languages/cpp || exit 1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build $B -j3 --target test_cpp_framework_manual_dispatch_owner > $J/kairos-mdo-build.log 2>&1; echo build rc=$?
LD_LIBRARY_PATH=/root/.cache/zlink/core-release/1.24.0/linux-x64/lib flock -s /root/jobs/fx/cx/measure.lock timeout 1500 $B/test_cpp_framework_manual_dispatch_owner --gtest_repeat=300 --gtest_brief=1 > $J/kairos-mdo-rep2.log 2>&1; echo rc=$?
grep -c "^\[  PASSED  \] 14" $J/kairos-mdo-rep2.log; grep -m3 -A8 "Failure" $J/kairos-mdo-rep2.log | cut -c1-200
