#!/bin/bash
set -u
cd /root/worktree/zlink-v21-s033/r4210/framework/languages/node
cp -al /root/worktree/zlink-v21-s033/s4190n/framework/languages/node/node_modules . 2>/dev/null || true
for p in /root/worktree/zlink-v21-s033/s4190n/framework/languages/node/packages/*/node_modules; do d=packages/$(basename $(dirname $p)); [ -e $d/node_modules ] || cp -al $p $d/ ; done
run() { node node_modules/typescript/bin/tsc -b tsconfig.build.json > /dev/null 2>&1 || node node_modules/typescript/bin/tsc -b tsconfig.build.json 2>&1 | tail -5; node --test --test-name-pattern="scheduled admission deadline" test/contract/location-outage-retention.test.js 2>&1 | grep -E "^# (pass|fail)|^not ok" ; node --test test/contract/location-runtime.test.js 2>&1 | grep -E "^# (pass|fail)"; }
echo "== after"; run
git checkout -q origin/main -- packages/framework/src/runtime/locations/runtime.ts
echo "== before"; run
git checkout -q HEAD -- packages/framework/src/runtime/locations/runtime.ts
git status --short | grep -v node_modules | head
