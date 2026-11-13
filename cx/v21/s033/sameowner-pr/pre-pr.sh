#!/bin/bash
W=/root/worktree/zlink-v21-s033/sameowner-impl
cd $W
V=$(sed -n "s/^ZLINK_BINDING_VERSION=//p" bindings/cpp/VERSION)
PRE_PR_CPP_CORE_PREFIX=/root/.cache/zlink/core-release/$V/linux-x64 /root/jobs/fx/pre-pr.sh $W; echo "pre-pr rc=$?"
