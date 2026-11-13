Model gpt-6.1-sol, effort high: this needs a concurrency judgement on close versus an in-flight native reply under load, and a choice between existing deadline mechanisms.

# Task: Java service port close must not fail because a direct reply is slow to leave (PR #4056, Refs #1639)

## Situation
PR #4056 (branch perf/1639-java-direct-reply, worktree /root/worktree/zlink-jdr, already committed; read `git diff origin/main...HEAD`) submits service replies directly through the binding. ZLinkJavaRawServicePort.replyMessages reads volatile closeAttempt once and rejects replies after close started; close() retries a BUSY router close every 1 ms, at most CLOSE_BUSY_MAX_ATTEMPTS = 8, then fails and keeps ownership.
Merge-queue CI failed (framework-java / linux-x64 / Java 25, run 42454160063): repliesRacingPortCloseAreRejectedAfterCloseStartsAndCloseSucceeds repetition 64/100 got ZlinkCloseException out of close.run() (ZLinkJavaRawServicePortContractTest.java:1043). A reply that passed the closed check and was then descheduled under load held the handle longer than 8 x 1 ms. This is a product defect: close can fail under load because of a fixed small time bound.
PM requirements:
- Reproduce the failure under load first (e.g. stress-ng on all CPUs or alongside a build), before changing code.
- Do not tie close to a fixed attempt count of 1 ms waits.
- Either wait until the in-flight submissions have left using an existing mechanism (an existing deadline such as the host shutdown deadline — see runtime/host/ZLinkFrameworkShutdown.java, ZLinkFrameworkRuntime.java:2737, ZLinkJavaSocketOptions.java FRAMEWORK_LINGER 30 s — or a binding/Core close mode), or prove structurally where the in-flight window ends.
- No new counter, lock or timer unless you explain why the existing mechanism cannot do it.
- After the fix, run the same test 1000 times under load and record the result.
Contracts: Core close returns EBUSY while an admitted API runs on the same handle, ESHUTDOWN after close is accepted (core/doc/spec/core/socket/README.ko.md:58-59, :647-658). Binding: reply after socket close is SubmitError(TERMINATED) (bindings/doc/spec/README.ko.md:1851). Existing Java contract (commit 3445ad6354): a close that fails returns the failure and keeps ownership for the next close.

## Do
1. Reproduce: run repliesRacingPortClose… and repliesRacingRuntimeClose… under load until it fails; record the command, load and the failing repetition.
2. Find which existing deadline or close mode the port close can use (file:line), and how port close learns it (constructor, host shutdown, socket option). Explain the choice. Keep the existing failure contract when that deadline expires.
3. Implement in framework/languages/java only. Update the tests that depend on CLOSE_BUSY_MAX_ATTEMPTS so they still prove: close succeeds after a slow in-flight reply; a close that stays BUSY past the deadline fails and keeps ownership; the next close cleans up; reentry runs once per port close() call (meshExplicitCloseReachesRetainedSocketOwner keeps reentrantCalls 2).
4. Run the two race tests 1000 times each under the same load; then `./gradlew :zlink-framework-core:test :zlink-framework-core:integrationTest` and `scripts/format/format.sh --check java`, `git diff --check`.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status (do not commit; the supervisor commits). No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/languages/java.
- Builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Load generation counts as a test: run it inside measure-shared.sh too, and stop it with SIGTERM when done.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work you see, with file:line, and whether your change removed it; no new queue, lock, cache, flag or state.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/jdr5/report.md (Korean, plain sentences): (1) reproduction command and result; (2) chosen mechanism with file:line and why; (3) decision-site count before/after; (4) 1000-run results under load and the gradle/format results with log paths. Save the diff on top of the branch as jdr5/change.diff.
