#!/bin/bash
# Repeat ZLinkLocalRequestAdmissionDeadlineTest at PR head, then at origin/main (main product + main tests).
cd /root/worktree/zlink-v21-s033/r4238/framework/languages/java
TR=zlink-framework-core/build/test-results/test
rep() { for i in $(seq 1 10); do rm -rf $TR; timeout 900 ./gradlew -q --offline :zlink-framework-core:test --tests '*ZLinkLocalRequestAdmissionDeadlineTest' >/dev/null 2>&1; echo "$1 run$i rc=$? $(grep -B1 '<failure\|<error' $TR/*.xml 2>/dev/null | grep -o 'testcase name="[^"]*"' | tr '\n' ' ')"; done; }
rep head
git checkout -q $(git -C /root/zlink rev-parse origin/main) -- zlink-framework-core/src
rep main
git checkout -q HEAD -- zlink-framework-core/src; git status --short | head -3
