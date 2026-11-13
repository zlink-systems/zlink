#!/bin/bash
set -u
cd /root/worktree/zlink-v21-s033/r4264/framework/languages/node
cp -al /root/worktree/zlink-v21-s033/s4190n/framework/languages/node/node_modules . 2>/dev/null || true
T="test/contract/idle-authority-recovery.test.js test/contract/startup-publication-recovery.test.js"
run() { node node_modules/typescript/bin/tsc -b tsconfig.build.json > /dev/null 2>&1 || node node_modules/typescript/bin/tsc -b tsconfig.build.json 2>&1 | tail -5; for t in $T; do echo "-- $t"; node --test $t 2>&1 | grep -E "^# (pass|fail)|^not ok" ; done; }
echo "== after"; run
B=5437ec7070434e0df639928baac1a028c0c1f88a
git checkout -q $B -- packages/framework/src
echo "== before (product at $B)"; run
git checkout -q HEAD -- packages/framework/src
git status --short | grep -v node_modules | head
