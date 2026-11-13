#!/bin/bash
cd /root/worktree/zlink-v21-s033/r4211/framework/languages/java
F=zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkLocationRuntime.java
t() { timeout 1500 ./gradlew -q --offline :zlink-framework-core:test --tests '*ZLinkLocationStoreOutageTest' --tests '*ZLinkLocationFirstHeartbeatTest' --tests '*ZLinkLocationRuntimeRecoveryTest' 2>&1 | grep -E "FAILED|tests completed|BUILD|error:" | head -12; echo "rc=${PIPESTATUS[0]}"; }
echo "== after"; t
git checkout -q origin/main -- $F
echo "== before"; t
git checkout -q HEAD -- $F; git status --short | head -3
