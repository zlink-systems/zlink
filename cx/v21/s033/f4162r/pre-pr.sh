#!/bin/bash
W=/root/worktree/zlink-v21-s033/f4162r
cd $W
V=$(sed -n "s/^ZLINK_BINDING_VERSION=//p" bindings/cpp/VERSION); P=/root/.cache/zlink/core-release/$V/linux-x64
PRE_PR_CPP_CORE_PREFIX=$P /root/jobs/fx/pre-pr.sh $W; echo "pre-pr rc=$?"
grep -E "실패|통과" /root/jobs/fx/pre-pr/perf/4162-latency-reference/summary.md 2>/dev/null | cut -c1-160
