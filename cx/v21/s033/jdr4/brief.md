# Task: Java direct reply — close quiescing with the existing closeAttempt (PM decision (가))

## Situation
Worktree /root/worktree/zlink-jdr3 (main 475305bebc + current change). Read /root/jobs/fx/cx/v21/s033/jdr3/report.md (previous step) and `git diff`.
Review finding on the previous step: `CLOSE_BUSY_MAX_ATTEMPTS=8` with `Thread.onSpinWait` lasts a few hundred ns, shorter than one native reply submit; with several threads replying continuously, close still failed (replyRacingPortClose iteration 6), and the test was changed to accept close failure. That is rejected.

PM decision:
- At the start of `replyMessages`, read the existing `closeAttempt` field once (make it `volatile`; no new field, no lane turn). If close has started, reject exactly as main does: `IllegalStateException("service port is closed")` (same type and message as main's `ensureOpen`), with the messages still consumed in finally. No other check is added to the reply path.
- Router close BUSY retry: up to 8 attempts, waiting 1 ms between attempts (e.g. LockSupport.parkNanos or Thread.sleep; justify). If still BUSY after the bound, the existing contract applies: failure to the caller, ownership kept, next close closes again.
- Revert the test change that accepted close failure: `replyRacingPortClose...` and `replyRacingRuntimeClose...` must assert close succeeds.

## Do
1. Implement the above. Keep the restored `failedSocketCloseRetainsOwnershipUntilLaterCleanupSucceeds` (BUSY past the bound) and the "BUSY clears after a few attempts" test; adjust attempt counts/timing only as the new wait requires, and say how.
2. New tests:
   a. Several threads reply continuously while close runs → close succeeds; replies that start after close began get "service port is closed"; every Message is consumed. Repeat 100 times (port close and runtime close).
   b. Deterministic order: a reply passes the closeAttempt check, then close starts and gets BUSY once (use completion signals / a test proxy around the router — no sleeps for ordering), then the reply's native submit leaves, and the retried close succeeds. Assert exactly one BUSY then success.
3. Run through `/root/jobs/fx/cx/measure-shared.sh`: :zlink-framework-core:test and :zlink-framework-core:integrationTest (cd framework/languages/java; ./gradlew --no-daemon --max-workers=2 ... -Pzlink.includeSamples=false), samples classes (`./gradlew --no-daemon --max-workers=2 -p samples classes`; if measure.lock is held by a measurement, wait for it — do not interrupt the wait), `scripts/format/format.sh --check java`, `git diff --check`. Run the new race tests 3 times in a row on their own as well.
4. Decision count for the PR body: reply submission judged where before (Framework lane + owner check + closed check, binding/Core) and after (Framework closed check only by one volatile read, binding/Core).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify bindings/ or core/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: no new queue, lock, cache, flag, timer or state; list constants added and why.
- Time box about 2 hours.

## Output: /root/jobs/fx/cx/v21/s033/jdr4/report.md (Korean, plain sentences): change, tests and their results (with the 3 repeated race runs), decision count. Save the final framework/ diff as /root/jobs/fx/cx/v21/s033/jdr4/change.diff.
