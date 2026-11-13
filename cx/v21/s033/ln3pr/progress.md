# Progress — PR #4415 Node L2/L3 A/B

- Measurement target: A = `6350275facc547759c9e143cc2515f3f528c776e` (origin/main), B = `afd3a0cea4eb732c9130a131f449bc7806600448` (PR #4415). Each worktree has the same six-file benchmark patch; production source is unmodified.
- Call shape: `call-shape.md` copied from the validated Node ladder S1 table; source send, target receive/reply and source completion route are the same; the L2 header and L3 service frame are the restored step work.
- Per-message costs: as recorded in `call-shape.md` § Per-message costs (L2 parses/encodes envelope headers; L3 packs/unpacks service frames and allocates mapped parts/frame wrappers; zero JavaScript atomics in the bench loop).
- Build: both worktrees used `npm ci` then `ZLINK_LOCAL_PACKAGE_ROOT=/root/jobs/fx/cx/v21/s033/ln3pr/package-root ZLINK_NODE_USE_BINDINGS_SOURCE=1 npm run build`; both completed with rc=0. The Node binding package was from `/root/.cache/zlink/packages/local-1.25.0/node/npm/zlink-systems-zlink-1.25.0.tgz`. Build outputs are under their own worktrees.
- A/B framework dist directory comparison: 803 files each; 15 generated outputs differ, matching the PR's 9 changed Node framework source files and TypeScript declaration/build metadata. Requested runtime fingerprints are among those differences. The three requested JS artifacts are newer than all package framework source files.
- Conditions: warmup 10 s; active 5 s; source CPU 0–5; target CPU 8–13; per-thread SCHED_OTHER and affinity recorded at start and mid-active. One discarded warm run before each A/B × L2/L3 counted run for every cell.
- Timing: each hold contains at most three cells and enforces a 9-minute stop boundary under exclusive `flock`; the measure lock is never waited on in queue mode.
2026-11-13T23:07:19Z running build found before hold; waiting up to 10 minutes
S    /bin/bash /root/jobs/fx/cx/test-slot.sh 8-15 ./gradlew --no-daemon --max-workers=2 :zlink-framework-core:test :zlink-framework-core:integrationTest -Pzlink.includeSamples=false
S    flock -n -E 211 /root/jobs/fx/cx/test.lock flock -s /root/jobs/fx/cx/measure.lock taskset -c 8-15 ./gradlew --no-daemon --max-workers=2 :zlink-framework-core:test :zlink-framework-core:integrationTest -Pzlink.includeSamples=false
S    flock -s /root/jobs/fx/cx/measure.lock taskset -c 8-15 ./gradlew --no-daemon --max-workers=2 :zlink-framework-core:test :zlink-framework-core:integrationTest -Pzlink.includeSamples=false
Sl   /usr/lib/jvm/java-25-openjdk-amd64/bin/java -Xmx64m -Xms64m -Dorg.gradle.appname=gradlew -jar /root/worktree/zlink-v21-s033/j4json/framework/languages/java/gradle/wrapper/gradle-wrapper.jar --no-daemon --max-workers=2 :zlink-framework-core:test :zlink-framework-core:integrationTest -Pzlink.includeSamples=false
S    awk $1 ~ /^[RS]/ && $0 ~ /(cmake|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ {print}
2026-11-13T23:08:32Z running build found before hold; waiting up to 10 minutes
Sl   java            /usr/lib/jvm/java-25-openjdk-amd64/bin/java -Xmx64m -Xms64m -Dorg.gradle.appname=gradlew -jar /root/worktree/zlink-v21-s033/j4json/framework/languages/java/gradle/wrapper/gradle-wrapper.jar --no-daemon --max-workers=2 -p samples classes
