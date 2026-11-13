# Task: Java direct reply — bound the close BUSY retry (PM decision)

## Situation
Worktree /root/worktree/zlink-jdr3 = main 475305bebc + the current change (also saved as /root/jobs/fx/cx/v21/s033/jdr2/change.diff). Read /root/jobs/fx/cx/v21/s033/jdr/report.md and /root/jobs/fx/cx/v21/s033/jdr2/report.md.
The current `closeRouterAfterInFlightCalls` in ZLinkJavaRawServicePort retries router close on CloseResult.BUSY forever. The PM decided:
- Retry BUSY inside the same CloseAttempt is approved, but NOT unbounded.
- Bound: retry only within an existing close/shutdown deadline value already present in the Framework Java runtime (no new timer, no new flag). If no such deadline exists, use a small constant retry count and state the reason in the report.
- Past the bound, the existing contract applies unchanged: the failure goes to the caller, ownership is kept, the next close closes again (commit 3445ad6354 behaviour).
- Test `failedSocketCloseRetainsOwnershipUntilLaterCleanupSucceeds` must be restored to its original name and original assertions (as on main 475305bebc) and still pass, now as "BUSY continues past the bound". Restore any other existing test whose expectations the previous fix changed for the same reason (check the mesh close test that changed attempts/reentrantCalls from 1 to 2 and the assertSame→assertNull change); if one cannot keep its original assertions, explain exactly why.
- New test: BUSY that clears after a few attempts → the same close succeeds.

## Do
1. Look for an existing close/shutdown deadline in the Java Framework runtime (ZLinkJavaRawServicePort, ZLinkJavaRawMeshNode, runtime options/config, CloseAttempt). Report file:line of what you found and which value you use, or that none exists.
2. Implement the bound. A test proxy that always returns BUSY must make close fail exactly as on main, with no wait longer than the bound. Keep Thread.onSpinWait or a cheaper wait; justify.
3. Keep the import cleanup in the test file; keep the race tests from the previous fix.
4. Run through `/root/jobs/fx/cx/measure-shared.sh`: :zlink-framework-core:test and :zlink-framework-core:integrationTest (cd framework/languages/java; ./gradlew --no-daemon --max-workers=2 ... -Pzlink.includeSamples=false), samples classes, `scripts/format/format.sh --check java`, `git diff --check`.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify bindings/ or core/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: no new queue, lock, cache, flag, timer or state; say whether a constant was added and why.
- Time box about 2 hours.

## Output: /root/jobs/fx/cx/v21/s033/jdr3/report.md (Korean, plain sentences): the bound and its basis with file:line, restored tests, new test, results. Save the final diff of framework/ as /root/jobs/fx/cx/v21/s033/jdr3/change.diff.
