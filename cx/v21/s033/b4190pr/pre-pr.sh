#!/bin/bash
W=/root/worktree/zlink-v21-s033/b4190pr
cd $W
[ -f .artifacts/wsl/npm/zlink-systems-zlink-1.24.0.tgz ] || { mkdir -p .artifacts/wsl/npm; cp /root/worktree/zlink-v21-s033/f4162/.artifacts/wsl/npm/zlink-systems-zlink-1.24.0.tgz .artifacts/wsl/npm/; }
PRE_PR_CPP_CORE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64 /root/jobs/fx/pre-pr.sh $W; echo "pre-pr rc=$?"
grep -E "실패|통과" /root/jobs/fx/pre-pr/bench/4190-submit-threads/summary.md 2>/dev/null | cut -c1-160
