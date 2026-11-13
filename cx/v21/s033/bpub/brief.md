Model gpt-6.1-sol, effort high: extends a concurrency fix (submit vs close ordering) to a socket type without the existing lifecycle owner; needs a careful ordering design and a load race test.

# Task: Java binding PUB publish vs close — the same released-handle window, fixed inside PR #4073 (#4060)

## Situation
PR #4073 (branch fix/4060-java-binding-submit-close, worktree /root/worktree/zlink-4060-binding-close, committed) fixed submit vs close for sockets with a CompletionOwner: a reservation count (`activeNativeCalls`, CompletionOwner.java) incremented under ownerLock after the closed check, released atomically; closeNativeSocket returns BUSY while reserved; one TERMINATED/ESHUTDOWN predicate. Read /root/jobs/fx/cx/v21/s033/bclose/report.md (the final design) and `git diff origin/main...HEAD -- bindings/java` in the worktree.
Gap left: PUB-type sockets have no CompletionOwner (SocketCore.java:58-62). SocketSendPlane.publishParts / publishNoWaitPartsResult (:225-240, submitters :265-294) call Native with socket.handle() after socket.ensureOpen() (SocketCore.java:261, a plain read of NativeSocketRuntime.handle). Close goes SocketCore.closeNativeHandle (:243-246, Native.close directly) then NativeSocketRuntime.closeInternal (:610-624) sets handle = NULL. So a publish racing close can pass a released handle to Core.
PM decision: each language PR includes the PUB publish gap; the PR body states whether it exists and where it is blocked.
Contracts: bindings/doc/spec/README.ko.md:1851 (submit after close gives SubmitError(TERMINATED)), :498 (binding adds no socket-local gate; concurrent independent records follow Core); core/doc/spec/core/socket/README.ko.md :58, :647, :657 (EBUSY while an admitted API runs; ESHUTDOWN after close accepted; released pointer must not be passed).

## Do
1. Trace every native call that uses the handle of a socket without a CompletionOwner on the publish path (publish, publish no-wait) with file:line, and list (report only, do not fix) other handle uses on such sockets that race close the same way (e.g. subscribe/unsubscribe, receive, options).
2. Design: one mechanism for both socket kinds. Prefer moving the existing reservation (count + closed predicate + BUSY in close) to the place both kinds share (e.g. SocketCore), with CompletionOwner using it, rather than a second copy. Publish must not serialize concurrent publishes (no socket-local gate, spec :498); at most one lock entry per publish where main had none, or a lock-free scheme with a written ordering argument. Keep one TERMINATED predicate. Explain any new field.
3. Tests: publish x close race on one PUB handle, 1000 repetitions under stress-ng load (load inside /root/jobs/fx/cx/measure-shared.sh, stop with SIGTERM), outcomes only success or TERMINATED; show the same test fails on the current branch before your change (count of INVALID_HANDLE/NOT_FOUND or other). Rerun the existing SubmitCloseRaceContractTest (1000 each) and SubmitCloseAdmissionContractTest, and the full binding suite (test integrationTest).
4. Write tests in the repository style (80 columns, 4-space continuation, existing import order; see neighbouring tests). Do not run google-java-format. git diff --check.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except bclose/; write only in bpub/ and this worktree.
- No git operations except git diff, git status, git log; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or other bindings. Changes in bindings/java only.
- Builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short. No single test loop longer than 40 minutes.
- 리팩토링 점검: list duplicate checks/handoffs you see with file:line and whether your change removed them.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/bpub/report.md (Korean, plain sentences): (1) publish path windows with file:line; other racing handle uses (report only); (2) design, reserve/release sites, lock entries per publish main vs after, decision-site count; (3) tests with log paths, before/after 1000-run outcomes; (4) duplicate work. Save the diff relative to the branch HEAD (including new files) as bpub/change.diff.
