#!/bin/bash
# kairos: trole-pr pre-pr after dropping stale 1.24.0 cache entries; i3541 after building the Node Core 1.25.0 package.
set -o pipefail
/root/jobs/fx/cx/v21/s033/trole-pr/pre-pr.sh > /root/jobs/fx/cx/v21/s033/trole-pr/pre-pr5.log 2>&1; echo "trole: $(tail -20 /root/jobs/fx/cx/v21/s033/trole-pr/pre-pr5.log | grep -E 'rc=|실패')"
W=/root/worktree/zlink-v21-s033/i3541
cd $W && /root/jobs/fx/cx/cpp-build-slot.sh bash scripts/local-package/node/build-wsl.sh --core-prefix /root/.cache/zlink/core-release/1.25.0/linux-x64 > /root/jobs/fx/cx/v21/s033/i3541c/node-pkg.log 2>&1; echo "node pkg rc=$?"; ls $W/.artifacts/wsl/npm/ 2>&1
/root/jobs/fx/cx/v21/s033/i3541c/pre-pr.sh > /root/jobs/fx/cx/v21/s033/i3541c/pre-pr3.log 2>&1; echo "i3541: $(tail -20 /root/jobs/fx/cx/v21/s033/i3541c/pre-pr3.log | grep -E 'rc=|실패')"
