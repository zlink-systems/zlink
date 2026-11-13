#!/bin/bash
W=/root/worktree/zlink-v21-s033/i3541
PRE_PR_CPP_CORE_PREFIX=/root/.cache/zlink/core-release/1.25.0/linux-x64 /root/jobs/fx/pre-pr.sh $W; echo "pre-pr rc=$?"
grep -E "실패|통과" /root/jobs/fx/pre-pr/fix/3541-core-io-thread-choice/summary.md 2>/dev/null | cut -c1-160
