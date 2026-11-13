Model gpt-6.1-sol, effort high: binding socket lifecycle under concurrency — a submit racing close must never pass a released native handle to Core; needs a careful ownership design and a load race test.

# Task: Java binding finishes submits on a closing/closed socket with TERMINATED, never touching the released handle (#4060)

## Situation
Issue #4060 (text below). In the Java binding, a reply().submit() racing close of the same RouterSocket reaches Core after the handle has been released: under load 1000 runs gave 4-7 INVALID_HANDLE/EFAULT and 9-17 NOT_FOUND/ENOENT. Found by the Framework race test in PR #4056 (framework/languages/java/.../ZLinkJavaRawServicePortContractTest, assertReplyRacingClose; the latest fixture is in /root/jobs/fx/cx/v21/s033/jdr6/change.diff).
Contracts:
- bindings/doc/spec/README.ko.md:1851: reply().submit() after the socket closed gives SubmitError(TERMINATED).
- core/doc/spec/core/socket/README.ko.md :657 new API after close accepted gives ESHUTDOWN; :656 and :1438 an invalid pointer gives EFAULT, a stale value ESTALE. Core rejects new calls with ESHUTDOWN only until the handle is released; after that the binding must not pass the handle.
- Core §2 (:43, :646-658): close returns EBUSY while an admitted API runs on the same handle.
Known code: bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:254 withNativeCall checks `closed` without ordering against close; NativeSocketRuntime.closeInternal (around :615) keeps the handle when close fails.
Done when (from the issue):
- send, request and reply submits on a closing or closed socket end with SubmitError(TERMINATED) without touching the native handle;
- the binding owns ordering between a submit already in progress and close (EBUSY or wait); callers need not know;
- a binding test: same-handle reply x close race under load, 1000 times, outcomes only success or TERMINATED;
- check whether the .NET, Node and C++ bindings have the same window (report file:line; do not fix them in this task unless trivial and the same pattern — say which).
Worktree: /root/worktree/zlink-4060-binding-close = latest main.

## Do
1. Trace the Java binding submit path (send/request/reply) and close path with file:line: where the handle is read, where Core is entered, where the handle is released, and every window between a closed check and Core entry. Quote the binding spec lines on close/submit ordering (rg bindings/doc/spec for TERMINATED, close, EBUSY).
2. Design: the smallest change in the binding's existing lifecycle owner (CompletionOwner / NativeSocketRuntime) so that a submit either enters Core while the handle is guaranteed alive (close then sees EBUSY from Core, which the binding already returns), or ends with TERMINATED without native access. Reuse existing state; explain any new field. No new queue or cache.
3. Tests in bindings/java: the reply x close race on one handle, 1000 repetitions under stress-ng load (run load inside /root/jobs/fx/cx/measure-shared.sh, stop with SIGTERM), outcomes only success or TERMINATED; plus send and request variants; plus existing binding suites in full.
4. Also run the Framework test from PR #4056 against the fixed binding if it can be wired locally (apply jdr6/change.diff to a scratch copy only; do not commit it), 1000 runs under load, and report outcomes.
5. Check .NET, Node, C++ bindings for the same window (file:line).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except jdr5/, jdr6/; write only in bclose/ and this worktree.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/. Changes in bindings/java (and test code); other bindings only if you state why.
- Builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`; C++ builds via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Run the Java format check for bindings if one exists (scripts/format/format.sh --check java covers bindings?) and git diff --check.
- 리팩토링 점검: list duplicate checks/handoffs you see with file:line and whether your change removed them.
- Time box about 5 hours.

## Output: /root/jobs/fx/cx/v21/s033/bclose/report.md (Korean, plain sentences): (1) path and windows with file:line and spec quotes; (2) design and decision-site count before/after; (3) tests with log paths and 1000-run outcomes; (4) other bindings check. Save the diff as bclose/change.diff.
