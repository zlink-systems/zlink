Model gpt-6-luna, effort high: the cause and the fixture lines are already pinned; this is a bounded test-fixture change plus a 1000-run check under load.

# Task: race tests must race only the responder close (PR #4056 follow-up)

## Situation
Read /root/jobs/fx/cx/v21/s033/jdr5/report.md. Worktree /root/worktree/zlink-jdr has the uncommitted deadline fix (keep the src/main changes as they are).
Under stress-ng load, close never failed in 3 x 1000 runs, but the reply-result assertion failed 3-5 times per 1000 with NOT_FOUND/ENOENT or BACKPRESSURED/EAGAIN. Cause: the fixture in ZLinkJavaRawServicePortContractTest (assertReplyRacingClose, around :1187-1273) opens the requester sockets on the same port, and close() closes sockets in reverse registration order, so requesters close before the responder. A reply admitted before the responder close then sees the requester gone, which Core reports as NOT_FOUND or a route-admission timeout (core/doc/spec/core/socket/README.ko.md:1256, :1262, :1267). That is a different situation from the one under test.

## Do
1. Change the fixture so the requesters live on a separate ZLinkJavaRawServicePort (or context) that the tested close does not touch; the tested close races only the responder. After asserting the reply results, close the requester port so outstanding requests end quickly instead of waiting for their 5 s timeout. Keep the allowed reply results as they are (success, "service port is closed", "socket is closed", SubmitError TERMINATED). Keep the diagnostic failure message jdr5 added.
2. Set both race tests back to @RepeatedTest(100) in the committed source. For the check, temporarily use 1000, run each under the same stress-ng load as jdr5 (run the load inside /root/jobs/fx/cx/measure-shared.sh, stop it with SIGTERM), record results, then restore 100.
3. Run `./gradlew --no-daemon --max-workers=2 :zlink-framework-core:test :zlink-framework-core:integrationTest`, `scripts/format/format.sh --check java`, `git diff --check`, all through measure-shared.sh.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status (do not commit). No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/, core/ or src/main. Changes only in the Java test source.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- Time box about 2 hours.

## Output: /root/jobs/fx/cx/v21/s033/jdr6/report.md (Korean, plain sentences): fixture change, 1000-run results under load for both tests with log paths, gradle/format results. Save the full uncommitted diff (main + test) as jdr6/change.diff.
