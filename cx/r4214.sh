#!/bin/bash
set -u
cd /root/worktree/zlink-v21-s033/r4214/framework/languages/node
cp -al /root/worktree/zlink-v21-s033/s4190n/framework/languages/node/node_modules . 2>/dev/null || true
run() { node node_modules/typescript/bin/tsc -b tsconfig.build.json > /dev/null 2>&1 || node node_modules/typescript/bin/tsc -b tsconfig.build.json 2>&1 | tail -5; node --test test/contract/user-spot-placement-lease.test.js 2>&1 | grep -E "^# (pass|fail)|^not ok" ; node --test test/contract/location-host.test.js 2>&1 | grep -E "^# (pass|fail)"; }
echo "== after"; run
git checkout -q origin/main -- packages/framework/src/runtime/host/index.ts
echo "== before"; run
git checkout -q HEAD -- packages/framework/src/runtime/host/index.ts
git status --short | grep -v node_modules | head
