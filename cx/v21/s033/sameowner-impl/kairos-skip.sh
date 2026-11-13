#!/bin/bash
# kairos: rebuild after the try_claim stale-entry skip, rerun focused tests x5 and unit/contract ctest.
J=/root/jobs/fx/cx/v21/s033/sameowner-impl W=/root/worktree/zlink-v21-s033/sameowner-impl B=$W/build-sameowner
P=/root/.cache/zlink/core-release/1.24.0/linux-x64
cd $W/framework/languages/cpp || exit 1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build $B -j3 -- -k 0 > $J/kairos-skip-build.log 2>&1 || { echo build failed; grep -m8 error: $J/kairos-skip-build.log; exit 1; }
echo build ok
F=$(grep -o "filters=.*" $J/run-focused.py | head -1)
LD_LIBRARY_PATH=$P/lib python3 $J/run-focused.py kairos-skip 5 > $J/kairos-skip-focused.log 2>&1; echo "focused rc=$?"; tail -5 $J/kairos-skip-focused.log
cd $B && LD_LIBRARY_PATH=$P/lib flock -s /root/jobs/fx/cx/measure.lock ctest -L '^(unit|contract)$' -j3 --timeout 900 > $J/kairos-skip-ctest.log 2>&1; echo "ctest rc=$?"; grep -E "tests passed|tests failed|\*\*\*Failed|Timeout" $J/kairos-skip-ctest.log | head -10
