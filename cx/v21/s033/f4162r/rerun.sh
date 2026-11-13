#!/bin/bash
# kairos: f4162r Node Core 1.25.0 package from the release prefix, restore provenance, merge main, pre-pr.
W=/root/worktree/zlink-v21-s033/f4162r J=/root/jobs/fx/cx/v21/s033/f4162r
cd $W || exit 1
/root/jobs/fx/cx/cpp-build-slot.sh bash scripts/local-package/node/build-wsl.sh --core-prefix /root/.cache/zlink/core-release/1.25.0/linux-x64 > $J/node-pkg.log 2>&1; echo "node pkg rc=$?"; ls .artifacts/wsl/npm/
git checkout -q -- bindings/node/provenance/core-package-provenance.json 2>/dev/null
git fetch -q origin main && git merge -q --no-edit origin/main && git log --oneline -1
git checkout -q -- bindings/node/provenance/core-package-provenance.json 2>/dev/null
git status --short | grep -v '^??' | head -3
$J/pre-pr.sh > $J/pre-pr3.log 2>&1; grep -E "rc=|실패|병합" $J/pre-pr3.log | tail -8
