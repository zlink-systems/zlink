#!/bin/bash
# PR 4268 Java #4260: head, then main/src at merge-base c820f2d3ba19f828b7ade0c15430250b2be11486 (renamed recovery test removed so the rest compiles).
cd /root/worktree/zlink-v21-s033/r4268/framework/languages/java
TR=zlink-framework-core/build/test-results/test
t() { rm -rf $TR; timeout 2400 ./gradlew -q :zlink-framework-core:test --tests '*ZLinkProviderAuthorityRepositoryTest' --tests '*ZLinkStoreLocationResolversTest' --tests '*ZLinkJavaRawMeshAuthorityCompletionTest' --tests '*StartupRecoveryTest' --tests '*InstanceSpotColdActivationTest' > /dev/null 2>&1; echo "rc=$?"
  for f in $TR/*.xml; do echo "$(basename $f .xml | sed 's/.*\.//') $(grep -o 'tests="[0-9]*" skipped="[0-9]*" failures="[0-9]*" errors="[0-9]*"' $f | head -1)"; done
  grep -B1 "<failure\|<error" $TR/*.xml 2>/dev/null | grep -o 'testcase name="[^"]*"' | head -20; }
echo "== head"; t
git checkout -q c820f2d3ba19f828b7ade0c15430250b2be11486 -- zlink-framework-core/src/main; rm -f /root/worktree/zlink-v21-s033/r4268/framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/locations/ZLinkInstanceActivationStartupRecoveryTest.java
echo "== product at c820f2d3ba19f828b7ade0c15430250b2be11486"; t
git checkout -q HEAD -- zlink-framework-core/src /root/worktree/zlink-v21-s033/r4268/framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/locations/ZLinkInstanceActivationStartupRecoveryTest.java 2>/dev/null; git clean -fdq zlink-framework-core/src; git status --short | head -3
