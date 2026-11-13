#!/bin/bash
cd /root/worktree/zlink-v21-s033/r4238/framework/languages/java
P=zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots
TR=zlink-framework-core/build/test-results/test
t() { rm -rf $TR; timeout 2400 ./gradlew -q --offline :zlink-framework-core:test --tests '*ZLinkLocalRequestAdmissionDeadlineTest' --tests '*ZLinkSpotTimerRegistryTest' --tests '*ZLinkSpotRuntime*' --tests '*SpotActivation*' > /dev/null 2>&1; echo "rc=$?"
  for f in $TR/*.xml; do echo "$(basename $f .xml | sed 's/.*\.//') $(grep -o 'tests="[0-9]*" skipped="[0-9]*" failures="[0-9]*" errors="[0-9]*"' $f | head -1)"; done | awk '{print}' | grep -v 'failures="0" errors="0"' ; echo "classes: $(ls $TR/*.xml | wc -l)"
  grep -B1 "<failure\|<error" $TR/*.xml | grep -o 'testcase name="[^"]*"' | head -20; }
echo "== head"; t
# mutation: drop cancellation forwarding at both new sites
perl -0pi -e 's/systems\.zlink\.framework\.runtime\.internal\.ZLinkCompletionBridge\s*\.forwardCancellation\(settled, admitted\);//' $P/ZLinkSpotActivation.java
perl -0pi -e 's/systems\.zlink\.framework\.runtime\.internal\.ZLinkCompletionBridge\.forwardCancellation\(\s*closed, result\);//' $P/ZLinkSpotRuntime.java
git diff --stat
echo "== mutated (no forwardCancellation)"; t
git checkout -q -- $P; git status --short | head -3
