#!/bin/bash
cd /root/worktree/zlink-v21-s033/f4162
V=$(sed -n "s/^ZLINK_BINDING_VERSION=//p" bindings/cpp/VERSION); P=/root/.cache/zlink/core-release/$V/linux-x64
[ -f .artifacts/wsl/npm/zlink-systems-zlink-$V.tgz ] || exit 1
ls -la .artifacts/wsl/npm/ | tail -3
PRE_PR_CPP_CORE_PREFIX=$P /root/jobs/fx/pre-pr.sh /root/worktree/zlink-v21-s033/f4162; echo "pre-pr rc=$?"
grep -E "실패|통과" /root/jobs/fx/pre-pr/perf/4162-bench-latency-stats/summary.md | cut -c1-160
