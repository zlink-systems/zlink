#!/bin/bash
# kairos: build client_server_runtime test after 250ms->5s (positive probe) and run --same-owner-regression and --same-owner-gated-control 10 times each.
W=/root/worktree/zlink-v21-s033/sameowner-impl B=$W/framework/languages/cpp/build/linux-ninja-release J=/root/jobs/fx/cx/v21/s033/sameowner-pr T=test_cpp_framework_client_server_runtime
P=/root/.cache/zlink/core-release/$(sed -n "s/^ZLINK_BINDING_VERSION=//p" $W/bindings/cpp/VERSION)/linux-x64
while pgrep -f "[s]ameowner-pr/t100.sh" >/dev/null; do sleep 20; done
cd $W/framework/languages/cpp || exit 1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build $B -j3 --target $T > $J/t250-build.log 2>&1 || { echo build failed; grep -m5 error: $J/t250-build.log; exit 1; }
t=$(find $B -name $T -type f -executable | head -1)
for mode in --same-owner-regression --same-owner-gated-control; do ok=0
  for i in $(seq 1 10); do LD_LIBRARY_PATH=$P/lib timeout 900 $t $mode > $J/t250${mode}-$i.log 2>&1 && ok=$((ok+1)) || echo "$mode run $i rc=$?"; done
  echo "$mode pass $ok/10"; grep -h "before release" $J/t250${mode}-1.log; done
