#!/bin/bash
cd /root/worktree/zlink-v21-s033/r4220/framework/languages/java
R=zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkStoreLocationResolvers.java
C=zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkClientServerLocationRuntime.java
TR=zlink-framework-core/build/test-results/test
t() { rm -rf $TR; timeout 1800 ./gradlew -q --offline :zlink-framework-core:test --tests '*ZLinkActorCreationCoordinatorTargetSelectionTest' --tests '*ZLinkClientServerM6ARuntimeTest' --tests '*ZLinkInstanceSpotActivationDeadlineTest' --tests '*ZLinkSpotRuntimeTargetSelectionTest' > /dev/null 2>&1; echo "rc=$?"
  for f in $TR/*.xml; do echo "$(basename $f .xml | sed 's/.*\.//') $(grep -o 'tests="[0-9]*" skipped="[0-9]*" failures="[0-9]*"' $f | head -1)"; done
  grep -B1 "<failure" $TR/*.xml | grep -o 'testcase name="[^"]*"' | head -20; }
echo "== after"; t
sed -i 's#return listMeshNodes(meshName).thenCompose(liveRows::meshNodes);#return listMeshNodes(meshName);#' $R
perl -0pi -e 's/reconcile\(\s*channelName,\s*candidates,\s*live,\s*epoch\)/reconcile(channelName, candidates, candidates, epoch)/' $C
git diff --stat
echo "== mutated (raw listing restored)"; t
git checkout -q -- $R $C; git status --short | head -3
