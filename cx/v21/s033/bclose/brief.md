Model gpt-6.1-sol, effort high: revising a lock-ordering design on the binding submit hot path (submit vs close reservation); concurrency correctness plus hot-path cost.

# Task: revise the Java binding submit/close reservation for #4060 (second pass)

## Situation
Worktree /root/worktree/zlink-4060-binding-close holds an uncommitted first-pass fix (also saved as /root/jobs/fx/cx/v21/s033/bclose/change-run1.diff; its report is bclose/report-run1.md — read both first).
First pass: CompletionOwner.withNativeCall (bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:260) takes ownerLock to check `closed` and increment `activeNativeCalls`, runs the native call, then takes ownerLock again to decrement. closeNativeSocket (:371) returns BUSY under ownerLock while the count is non-zero. Results were correct (1000-run races only OK/TERMINATED).
Supervisor review found two problems to fix:
1. Hot-path cost. Every submit now enters ownerLock twice more. trackNoWaitSend (:279, the DONT_WAIT send path the Framework uses) already enters ownerLock in nextContextToken, so it now takes the lock 3 times per message where main takes it once; other submit paths (nextSendContextToken, pending registration) likewise. The completion drain also takes ownerLock, so this adds contention. Required: each submit path enters ownerLock no more times than on main. Take the reservation in the same ownerLock section that already checks `closed` and allocates the token (or registers the pending record), and release it without ownerLock (e.g. an atomic decrement), while close still reads the count under ownerLock. Increments must only happen under ownerLock with closed == false, so close cannot miss one; a stale non-zero read only gives a BUSY that the caller retries. Every path that reserves must release on every exit (exceptions before the native call included) — show each with file:line. Paths with no existing ownerLock section may keep one lock entry for the reservation. Keep one TERMINATED predicate (decision sites stay 1).
2. Formatting. RequestRetryCloseContractTest.java was reformatted wholesale (100 columns, 8-space continuation, import reorder) which does not match the repository style (look at the original file and neighbouring tests in bindings/java/src/test: 80 columns, 4-space continuation, existing import order). Revert unchanged lines to their original form and keep only the semantic change; write the two new tests (SubmitCloseAdmissionContractTest, SubmitCloseRaceContractTest) in the same repository style. Do not run google-java-format.
Also report: SocketSendPlane.publishParts / publishNoWaitPartsResult (:225-240) call Native on PUB-type sockets, which have no CompletionOwner (SocketCore.java:58). Does the same released-handle window exist for publish vs close? Report file:line only; do not fix.

## Do
1. Implement the revision above in CompletionOwner (and callers only if needed).
2. Measure hot-path cost: count ownerLock entries per submit path before (main) and after, by code reading with file:line. No timing benchmarks.
3. Rerun: the binding race tests 1000 times each under stress-ng load (load inside /root/jobs/fx/cx/measure-shared.sh, stop with SIGTERM), outcomes only OK/TERMINATED; the admission test; the full binding suite; and the Framework race from jdr6 1000 runs as in the first pass (scratch copy only, same method as report-run1.md).
4. git diff --check.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except jdr6/; write only in bclose/ and this worktree.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or other bindings.
- Builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short. No long full-suite runs with -j1 or single-threaded loops longer than 40 minutes.
- 리팩토링 점검: list duplicate checks/handoffs you see with file:line and whether your change removed them.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/bclose/report.md (Korean, plain sentences): (1) design, each reserve/release path with file:line; (2) ownerLock entries per submit path, main vs after; (3) tests with log paths and 1000-run outcomes; (4) publish vs close window. Save the full diff (including new files) as bclose/change.diff.
