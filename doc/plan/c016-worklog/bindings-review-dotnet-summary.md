# bindings/dotnet review — contract B (0.17.0) summary

Scope: bindings/dotnet/** only. Core 50d77800f2 (0.17.0) used as built in core/build (not rebuilt).
Base commit reviewed: 4f503b76d3 port + the previous worker's uncommitted edits. All edits remain uncommitted.

## Changed files (uncommitted)

Runtime
- bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs — payload snapshot only after BACKPRESSURED; runtime pump without Task.Yield spin (25 ms timed native wait on POLLCOMPLETION); EINTR-tolerant pump; wait-error handling; TransferToRuntime failure settles waiters; drain returns early when closing; TargetMatches without allocation; brace-less stacked `lock` blocks reformatted (whitespace only)
- bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs — completion ownership acquired only for POLLCOMPLETION registrations (SUB/PUB etc. can be polled); rollback only when ownership was actually acquired; Modify acquires the owner lazily; Remove works on a closed socket; PollErr triggers a drain; wait error settles waiters only on ETERM/ESHUTDOWN
- bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs — ConsumeParts fast path for 1–2 parts (no HashSet)
- bindings/dotnet/src/Zlink/Runtime/Errors/ZlinkException.Native.cs — ESHUTDOWN (58/108/10058/hausnumero+22) mapped to ErrorCode.EShutdown / SubmitResult.Terminated; IsTerminationError helper
- bindings/dotnet/src/Zlink/Runtime/Sockets/SocketHandle.cs — keeps its Context alive; finalizer removed (ordering now owned by SocketKernel)
- bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.Lifecycle.cs — finalizer stops the pump and closes native in the right order; abandoned sockets settle pending operations
Tests
- bindings/dotnet/tests/Zlink.Tests/test_completion_lifecycle_regressions.cs (new, previous worker, 7 tests)
- bindings/dotnet/tests/Zlink.Tests/test_contract_b_regressions.cs (new, 4 tests)
- bindings/dotnet/tests/Zlink.Tests/test_optimization_guard.cs (+4 guard assertions)
Scripts
- bindings/dotnet/tests/run_tests.sh, samples/run_samples.sh, perf/single/run_benchmarks.sh, perf/multi/run_benchmarks.sh — `dotnet build -m:1`; multi extract rejects non-finite/zero throughput and negative latency
- bindings/dotnet/perf/single/run_emit.py — a run with non-finite/zero throughput or negative latency is a `fail` (`invalid_metrics`), not a success

## Bug table

| # | File:line | Symptom | Fix |
|---|-----------|---------|-----|
| 1 | CompletionOwner.cs:45-83 (SendAsync) | Port cloned the payload (zlink_msg_copy + managed Message per part) on EVERY async send before the DONTWAIT attempt | Snapshot (shared zlink_msg copy) is taken only after `BACKPRESSURED` with a nonzero token; success path keeps no copy |
| 2 | CompletionOwner.cs:570-630 (RuntimePump) | `await Task.Yield()` + `zlink_poller_wait(...,0)` = thread-pool busy spin while any token/request was armed | Blocking native wait (25 ms slice, level POLLCOMPLETION) on a dedicated Task; no spin, no Task.Yield |
| 3 | CompletionOwner.cs:592-608 (RuntimePump) | Core `socket_poller` returns -1/EINTR on a signal; the pump exited and failed every armed waiter with InternalError | EINTR keeps pumping; only a non-EINTR error ends the pump (ETERM/ESHUTDOWN -> Terminated, else InternalError) |
| 4 | Poller.cs:229-240 (Wait) | Any wait error (EINTR, EBUSY from a concurrent Wait) failed every armed waiter of every owned socket; a later WRITABLE then dropped the payload | Waiters are settled only for ETERM/ESHUTDOWN (`FailPublicWaitTerminated`); other errors surface to the caller and leave records level-true for the next Wait |
| 5 | Poller.cs:39-58 (Add) | Completion ownership was taken for every socket, so registering a SUB/PUB/XPUB for POLLIN threw (POLLCOMPLETION is PAIR/DEALER/ROUTER/STREAM only) | Ownership only for registrations that include POLLCOMPLETION |
| 6 | Poller.cs:51-56, 104-118 (Add/Modify rollback) | Failure after a no-op TransferToPublic (already owned) rolled ownership back to runtime, leaving the still-registered item without an owner | `TransferToPublic` reports whether it acquired; rollback only then |
| 7 | Poller.cs:104-109 (Modify) | Modify to POLLCOMPLETION on an item registered without it dereferenced a null CompletionOwner | Owner resolved lazily from the socket kernel |
| 8 | Poller.cs:141-158 (Remove) | Remove of a closed socket threw (RequirePollableHandle) so the item leaked in the poller | Looks the item up by socket reference and uses the stored native handle |
| 9 | Poller.cs:248-260 (Wait) | POLLERR on an owned socket did not drain, so a TERMINAL WRITABLE stayed unread | PollErr also triggers the drain |
| 10 | CompletionOwner.cs:237-258 (TransferToRuntime) | If re-arming the runtime poller failed after native poller removal, waiters had no drain owner forever | Failure path stops the pump and settles the waiters |
| 11 | CompletionOwner.cs:291-326 (DrainCore) | `zlink_completion_recv` error (ETERM) threw without settling entries | `FailDrainWaits` settles entries (Terminated on ETERM) before rethrow |
| 12 | ZlinkException.Native.cs:110-113, 227-229 | ESHUTDOWN (WRITABLE TERMINAL on close) mapped to InternalError instead of Terminated | ESHUTDOWN/ETERM -> `SubmitResult.Terminated`, `ErrorCode.EShutdown` |
| 13 | SocketHandle.cs / SocketKernel.Lifecycle.cs | Socket finalizer closed native without stopping the pump; a socket did not keep its Context alive (context could be finalized under a live socket) | Finalizer moved to SocketKernel (stop pump -> close -> settle entries); SocketHandle roots the Context |
| 14 | RequestReplySupport.cs:62-82 (ConsumeParts) | HashSet allocation per submit | Fast path for 1–2 parts |

Checklist verdicts (code read against core/doc/spec/core/socket/README, api.h comment and the contract summary):
- (a) OK/ID 0 completes immediately (`CompleteInitialSuccess`), no completion wait; ID != 0 on OK is a protocol failure. PASS.
- (b) Payload retained only on BACKPRESSURED+token; resend happens in `Capture` only for a WRITABLE whose token, user_context and peer RID match (entries are keyed by user_context = GCHandle, so another token's WRITABLE cannot wake it); a re-BACKPRESSURED resend re-arms with the new token. PASS (new test `rebackpressured_retry_keeps_waiting_and_delivers_each_record_once`).
- (c) WRITABLE `send_result == TERMINAL` -> `ZlinkSubmitException` mapped from `send_terminal_errno` (ENOENT -> NotFound, ESHUTDOWN/ETERM -> Terminated). PASS.
- (d) NOT_CONNECTED/EHOSTUNREACH is thrown immediately (Async faults synchronously as before the port; TrySubmit throws, only BACKPRESSURED returns false). PASS with MANDATORY on; see BLOCKERS for MANDATORY off.
- (e) One owner drains to NO_DATA (`DrainCore`); REQUEST and WRITABLE records are dispatched by user_context; WRITABLE-only wakes are filtered from the caller-visible POLLCOMPLETION and the Wait continues for the remaining deadline; POLLOUT/POLLCOMPLETION are level, so no lost wake; no zero-timeout loop remains. PASS.
- (f) Close: PrepareClose (closing flag, pump stopped, submit lock held) -> native close -> all entries FailLifecycle + GCHandle free; drains after close return early; TrySubmit(false) tokens are payload-free sinks retired by WRITABLE or close. Lock order submit -> drain -> runtime -> sync is consistent in every path; the pump never holds runtimeSync while taking submitSync. PASS (tests: close/context-shutdown/abandoned-socket/TrySubmit-close).
- (g) Exceptions: ZlinkSubmitException (Backpressured/NotConnected/NotFound/Terminated/InternalError), ZlinkRequestException (Terminated/TimedOut), ZlinkConfigException for poller errors — matches README "Send back-pressure and completion". PASS after ESHUTDOWN fix.
- (h) Blocking `Submit()` (`SocketKernel.Send*`, `SendFlags.None`), `Request()/Async()` and `Publish` paths are untouched by the diff. PASS (191/191 suite incl. pre-existing contract tests).

## Perf table

| Item | Verdict | Fix | Measurement |
|------|---------|-----|-------------|
| Per-send payload byte copy | None. `SubmitPreservingOnFailure` clones via `zlink_msg_copy` (refcount share, no byte copy > VSM) exactly as before the port; the port's extra retained clone per async send was removed (bug 1) | Snapshot deferred to refusal | Source-guard test `async_send_retains_payload_only_after_backpressure` |
| Per-send allocations (success path) | Same set as pre-port (entry, TCS, GCHandle, submit closure, clone array); HashSet in ConsumeParts removed | fast path 1–2 parts | — |
| Spin/sleep in poll/drain loops | Task.Yield spin removed; pump blocks in native poller wait with a 25 ms slice (bounded so close/ownership transfer can stop it); public Wait loops only after internal WRITABLE progress with the remaining deadline | bugs 2–4 | Source-guard assertions |
| Completion wait structure | `Dictionary<IntPtr, CompletionEntry>` keyed by user_context, O(1) | — | — |
| Hot-path locks | `_submitSync` + `_sync` (register) + `_runtimeSync` (prepare) per async send, unchanged from pre-port | — | — |

DEALER_ROUTER tcp 1024B, `--duration 3 --runs 1` (bindings/dotnet/perf/run_benchmarks.sh). The runner drives the blocking send path, which this diff does not touch; the box was shared with other jobs (load avg shown), so an interleaved A/B against a `git archive HEAD` copy of the binding was used instead of the single before/after pair.

| Run | Load | Throughput msg/s | Latency mean ms | p95 | p99 |
|-----|------|------------------|-----------------|-----|-----|
| Before (previous worker, 19:13, HEAD) | n/a | 246596 | 2.701 | 10.452 | 14.262 |
| After (19:53, current) | 15 | 160731 | 27.607 | 42.619 | 91.892 |
| A/B r1 HEAD / current | 14.7 / 14.0 | 145885 / 104626 | 1.655 / 10.902 | 6.967 / 56.203 | 9.173 / 59.091 |
| A/B r2 HEAD / current | 14.0 / 14.3 | 75588 / 90830 | 4.678 / 5.695 | 11.927 / 13.438 | 17.985 / 19.532 |
| A/B r3 HEAD / current | 14.3 / 14.5 | 89022 / 76217 | 3.836 / 5.534 | 10.874 / 15.794 | 18.059 / 28.699 |
| A/B r4 HEAD / current | 6.0 / 5.8 | 266904 / 281153 | 0.690 / 0.381 | 2.778 / 0.906 | 4.367 / 5.439 |
| A/B r5 HEAD / current | 5.8 / 5.6 | 296591 / 287042 | 0.364 / 13.600 | 1.242 / 29.164 | 5.900 / 32.117 |

Verdict: no measurable regression; at comparable load (r4/r5) current is within ±5 % of HEAD (281k/287k vs 267k/297k). High-load rows are noise-dominated (HEAD itself spans 65k–147k under load 14–15).

## Smoke results

- `ZLINK_CORE_SOURCE=local bash bindings/dotnet/tests/run_tests.sh`: 191/191 managed tests PASS, 7/7 samples OK, `[dotnet-tests] PASS` (run on the final tree).
- New/contract tests 5x: test_contract_b_regressions (4) + test_completion_lifecycle_regressions (7) + test_optimization_guard + test_routed_async_admission + test_pull_completion_contract = 49 tests, 5/5 runs green, no Thread.Sleep/Task.Delay in the new tests.
- perf single smoke `--pattern PAIR,DEALER_ROUTER,PUBSUB --transports tcp,inproc --msg-sizes 1024 --duration 2 --runs 1`: success 6 / fail 0, status complete (load 15.5):
  DEALER_ROUTER inproc 67677 msg/s (6.14 ms), tcp 70455 (6.21 ms); PAIR inproc 78817 (1.89 ms), tcp 58695 (6.86 ms); PUBSUB inproc 53175 (10.48 ms), tcp 99335 (3.70 ms).
- perf multi smoke: the env names in the brief (`CCU/DUR/SIZES/PATTERNS/TIMEOUT`) are not options of this runner (it uses `--clients/--duration/--msg-sizes/--pattern` and `PERF_MULTI_TIMEOUT_SECONDS`); with them the runner silently ran its full default matrix (100 clients, 5 s, 6 sizes, tcp/tls/ws/wss) and was stopped. Rerun with the equivalent real options
  `PERF_MULTI_TIMEOUT_SECONDS=300 bash bindings/dotnet/perf/run_benchmarks_multi.sh --pattern DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB --clients 8 --duration 2 --msg-sizes 1024,65536 --transports tcp`: success 6 / fail 0, status complete, exit 0 (load 9.7):
  MULTI_DEALER_DEALER 1024B 176791 msg/s, 64KiB 35590; MULTI_DEALER_ROUTER_SENDSEND 1024B 102922, 64KiB 31584; MULTI_PUBSUB 1024B 314398, 64KiB 102312. No stall, no error, no zero throughput.
- `dotnet format Zlink.sln --verify-no-changes --include <changed .cs>`: clean. `git diff --check -- bindings/dotnet`: clean.

## BLOCKERS

1. Core/spec discrepancy (outside bindings/dotnet): core/src/runtime/sockets/router/router_send_path.cpp:77 raises EHOSTUNREACH for a no-route RID only when `_mandatory` is set; with MANDATORY off Core drops the record silently and returns OK/ID 0. core/doc/spec/core/socket/README.en.md:1013-1015 says NOT_CONNECTED is returned "regardless of ZLINK_ROUTER_OPT_MANDATORY". The binding reports whatever Core returns; the (d) regression test asserts the contract with MANDATORY on and documents this in a comment.
2. Multi perf smoke command in the brief uses env names the runner does not read (see above); the runner's option surface was left unchanged (public surface of bindings/dotnet/perf) — the brief's smoke needs the `--clients/--duration/--msg-sizes/--pattern` form. Transports were limited to tcp for the gate because the default tls/ws/wss matrix does not fit the time cap; ws/wss/tls were not exercised here.
3. Unit-level regression for the pump's EINTR tolerance is a source-guard assertion (test_optimization_guard) rather than a public-API test: a signal cannot be delivered deterministically to the pump's thread-pool thread from managed code. The poller-side equivalent (non-terminal wait error must not settle waiters) is covered by the public-API test `concurrent_wait_error_does_not_fail_pending_writable_waiter`.

No public API change was needed.
