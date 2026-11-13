#!/bin/bash
cd /root/worktree/zlink-v21-s033/r4266/framework/languages/java
git checkout -q 5437ec7070434e0df639928baac1a028c0c1f88a -- zlink-framework-core/src/main
timeout 1200 ./gradlew :zlink-framework-core:compileTestJava > /root/jobs/fx/cx/r4266-basecompile.log 2>&1; echo "rc=$?"
grep -E "error:" /root/jobs/fx/cx/r4266-basecompile.log | sed 's|.*/src/test/java/||' | head -30
grep -c "error:" /root/jobs/fx/cx/r4266-basecompile.log
git checkout -q HEAD -- zlink-framework-core/src/main; git clean -fdq zlink-framework-core/src/main; git status --short | head -3
