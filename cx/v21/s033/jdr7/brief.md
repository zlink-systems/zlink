Model gpt-6-luna, effort high: the PM fixed the exact shape (one package-private constructor parameter with a default); this is a bounded Java change plus test updates.

# Task: inject the Java service port close deadline (PR #4056)

## Situation
Worktree /root/worktree/zlink-jdr (branch perf/1639-java-direct-reply) has uncommitted changes from two earlier steps (read `git diff` and /root/jobs/fx/cx/v21/s033/jdr5/report.md, jdr6/report.md). ZLinkJavaRawServicePort.close() now computes one deadline per close call from ZLinkFrameworkRuntime.DEFAULT_TERMINATION_DEADLINE (30 s) and retries BUSY router close until it.
PM decision: the deadline is a setting, not state. Add exactly one field: a package-private constructor parameter (default DEFAULT_TERMINATION_DEADLINE for every existing constructor) that close() uses instead of the constant. Tests that expire the deadline while BUSY must use a short deadline (tens of ms) instead of waiting 30 s.
The reply x close race tests still fail occasionally because of a binding defect (#4060, handled by another job). Do not change their allowed results and do not try to fix that here.

## Do
1. Add the constructor parameter and field; keep all existing constructors and callers working with the default. Use it in close().
2. Change failedSocketCloseRetainsOwnershipUntilLaterCleanupSucceeds and meshExplicitCloseReachesRetainedSocketOwner (and any other test that waits the real deadline) to use a short injected deadline; they must still prove: failure returned and ownership kept while BUSY past the deadline, the next close cleans up, reentrantCalls 2.
3. Run `./gradlew --no-daemon --max-workers=2 :zlink-framework-core:test --tests '*ZLinkJavaRawServicePortContractTest*'` and report per-test times; then `scripts/format/format.sh --check java` and `git diff --check`. All via /root/jobs/fx/cx/measure-shared.sh.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except jdr5/ and jdr6/; write only in jdr7/ and this worktree.
- No git operations except git diff and git status (do not commit). No spec edits; no GitHub operations. Do not read core/include. Do not run scripts/dev/internal-docs.sh. Do not modify bindings/ or core/. Changes only in framework/languages/java.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- Time box about 1.5 hours.

## Output: /root/jobs/fx/cx/v21/s033/jdr7/report.md (Korean, plain sentences): the field, constructors touched, test changes, test results with times and log paths. Save the full uncommitted diff as jdr7/change.diff.
