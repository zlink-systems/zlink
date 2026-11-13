#!/bin/bash
# PR 4266 Java prestart: new tests at head, then with main/src reverted to merge-base 5437ec7070434e0df639928baac1a028c0c1f88a.
cd /root/worktree/zlink-v21-s033/r4266/framework/languages/java
TR=zlink-framework-core/build/test-results/test
t() { rm -rf $TR; timeout 2400 ./gradlew -q --offline :zlink-framework-core:test --tests '*ZLinkPrestartCallLifecycleTest' --tests '*ZLinkPrestartCallReproTest' --tests '*EntrySpotActorDispatchTests' --tests '*ZLinkDefaultSpotOutboundTerminalTest' > /dev/null 2>&1; echo "rc=$?"
  for f in $TR/*.xml; do echo "$(basename $f .xml | sed 's/.*\.//') $(grep -o 'tests="[0-9]*" skipped="[0-9]*" failures="[0-9]*" errors="[0-9]*"' $f | head -1)"; done
  grep -B1 "<failure\|<error" $TR/*.xml 2>/dev/null | grep -o 'testcase name="[^"]*"' | head -15; }
echo "== head"; t
git checkout -q 5437ec7070434e0df639928baac1a028c0c1f88a -- zlink-framework-core/src/main
echo "== product at 5437ec7070434e0df639928baac1a028c0c1f88a (tests from head)"; t
git checkout -q HEAD -- zlink-framework-core/src/main
git status --short | head -3
