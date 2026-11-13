#!/bin/bash
# PR 4266: product-only revert (core + spring starter src/main; added files deleted), ReproTest only. Tests from head.
W=/root/worktree/zlink-v21-s033/r4266; MB=5437ec7070434e0df639928baac1a028c0c1f88a
J=framework/languages/java; P="$J/zlink-framework-core/src/main $J/zlink-framework-spring-boot-starter/src/main $J/zlink-framework-core/src/test"
KEEP=$J/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/host/ZLinkPrestartCallReproTest.java
cd $W || exit 1
git diff --name-status $MB HEAD -- $P | grep -v ZLinkPrestartCallReproTest.java > /root/jobs/fx/cx/r4266c-files.txt
awk '$1=="A"{print $2}' /root/jobs/fx/cx/r4266c-files.txt | xargs -r rm -f
awk '$1!="A" && $1 !~ /^R/{print $2} $1 ~ /^R/{print $2}' /root/jobs/fx/cx/r4266c-files.txt | xargs -r git checkout -q $MB --
awk '$1 ~ /^R/{print $3}' /root/jobs/fx/cx/r4266c-files.txt | xargs -r rm -f
echo "reverted: $(wc -l < /root/jobs/fx/cx/r4266c-files.txt) files"
cd $J
TR=zlink-framework-core/build/test-results/test; rm -rf $TR
timeout 2400 ./gradlew :zlink-framework-core:test --tests '*ZLinkPrestartCallReproTest' > /root/jobs/fx/cx/r4266c-gradle.log 2>&1; echo "rc=$?"
grep -c "error:" /root/jobs/fx/cx/r4266c-gradle.log; grep "error:" /root/jobs/fx/cx/r4266c-gradle.log | sed 's|.*/src/||' | cut -d: -f1 | sort -u | head
for f in $TR/*.xml; do [ -f "$f" ] && echo "$(basename $f .xml | sed 's/.*\.//') $(grep -o 'tests="[0-9]*" skipped="[0-9]*" failures="[0-9]*" errors="[0-9]*"' $f | head -1)"; done
grep -h -o 'testcase name="[^"]*"' $TR/*.xml 2>/dev/null | wc -l
cd $W && git checkout -q HEAD -- $P && git clean -fdq $P && git status --short | head -3
