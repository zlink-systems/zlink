#!/bin/bash
# kairos: build receive_role_handoff test after 100ms->5s and run it 20 times with load from the other builds.
W=/root/worktree/zlink-v21-s033/sameowner-impl B=$W/framework/languages/cpp/build/linux-ninja-release J=/root/jobs/fx/cx/v21/s033/sameowner-pr T=test_cpp_framework_receive_role_handoff
P=/root/.cache/zlink/core-release/$(sed -n "s/^ZLINK_BINDING_VERSION=//p" $W/bindings/cpp/VERSION)/linux-x64
cd $W/framework/languages/cpp || exit 1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build $B -j3 --target $T > $J/t100-build.log 2>&1 || { echo build failed; grep -m5 error: $J/t100-build.log; exit 1; }
t=$(find $B -name $T -type f -executable | head -1); ok=0
for i in $(seq 1 20); do LD_LIBRARY_PATH=$P/lib timeout 600 $t > $J/t100-run-$i.log 2>&1 && ok=$((ok+1)) || echo "run $i rc=$?"; done
echo "pass $ok/20"; grep -E "^\[  (PASSED|FAILED) " $J/t100-run-1.log
