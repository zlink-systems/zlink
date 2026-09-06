# Node intermittent gate fixes

Both failures reproduced and were fixed at the test-fixture boundary (classification **B — existing defects**). Each complete affected test file passed 300/300 runs on the final code. The final npm gate completed with four unrelated failures in two sample-runner test files; all sample self-checks passed. Public API and production timer policies are unchanged. M6A production runtime was not modified; Spot runtime changes only pass an internal clock dependency to its existing timer owner. No commit was made.

Evidence directory: `/tmp/zlink-node-intermittents.mJYUn0/`. All test invocations held `/tmp/zlink-node-gate.lock`; the final `npm test` also held `/tmp/zlink-samples-gate.lock`. Temporary diagnostic/mutation preload scripts exist only in that evidence directory and are not part of the diff.

## Failure 1: M6A bilateral endpoint requests

### Root cause and captured state

The original fixture stopped progressing both peers after its initial Ready check. Its request loop (`framework/languages/node/test/m6a/m6a-runtime.contract.ts:1958`, before this change) pumped only the target and did not run liveness. A delayed admission could invalidate that initial readiness before the reverse request.

The unmodified targeted test passed **200/200**. A subsequent planned 200-run full-M6A reproduction stopped on the first failure, **run 123**, with the original test 39 timeout. Evidence: `m6a-baseline.results`, `m6a-suite.results`, `m6a-suite-123.log:232`, and `m6a-suite-123.state`.

| Captured transition | State |
| --- | --- |
| Right admits Left at monotonic 245.411 ms | `unmonitored:inbound:2677620b-8735-4abb-b6ae-4e66f56cb927`; both descriptors have `objectRole: server` |
| Right observes actual READY at 248.992 ms | Peer RID `m6a-endpoint-left`, connection ID `177`, `transportLane: 0`; initiator remains `m6a-endpoint-left` |
| Right processes a delayed Hello at 252.025 ms | Admission replaces the provisional identity with `["m6a-endpoint-left","connection","177"]`; new liveness entry has `ready: false` and an immediately due first probe |
| Left → Right request completes at 253.427 ms | `terminalResult: 0`, payload `BilateralAnswer` |
| Right → Left request completes at 253.891 ms | `terminalResult: 109` (`NotConnected`), `failureCode: 0`; right liveness still false, operation registry empty |

The reverse request was **never submitted to a socket**: `raw-service-mesh-runtime.ts:1213` completes a known but non-ready target locally and returns before `router.request` at line 1233. Thus there was no lost application record or stranded WRITABLE token on another lane in this reproduction. Both routers remained open; the application mailboxes were empty after the first request was claimed/released. The test waited for an application record despite its pending operation already having completed with NotConnected.

The preceding transition follows existing runtime ownership: `drainMonitorEvents` selects the provisional physical candidate at `raw-service-mesh-runtime.ts:1057`; admission calls the liveness owner at line 1317; `service-liveness-registry.ts:42` requires fresh readiness evidence when the connection identity changes. `service-topology-registry.ts:172` explicitly allows monitored evidence to supersede an unmonitored placeholder.

### Fix and regression

`verifyBilateralEndpointRequests` at `test/m6a/m6a-runtime.contract.ts:1913` uses one progress function for both readiness and application delivery: each peer advances admission announcements, monitor events, ordinary receive, and liveness. Each direction submits exactly one request after observing current Ready and the required initiator. Delivery is observed from the owned mailbox, preserving the received record instead of relying on a transient pump return value. All original RID, initiator, reply-success, and disconnect assertions remain. `pollUntil` uses `performance.now()` with the original 2,000 ms bound (D-095).

The new regression at line 1898 replays the captured loss of liveness readiness through the existing liveness registry after the first reply, then requires a successful reverse request. It retains real native request/reply transport. A temporary in-memory mutation restoring the original one-time readiness checkpoint makes this regression fail with the original deterministic-progress timeout: `m6a-regression-old-fixture.log`. No retry, resubmission, added sleep, or timeout increase is used.

Alternatives considered: retain the raw-runtime contract test with a complete deterministic pump, or replace it with a hosted-backend integration test. The former preserves the test's layer and keeps progress ownership explicit without changing runtime admission policy.

Ownership: test fixture owns manual progress; Framework's existing liveness registry owns logical readiness; Core/binding continue to own physical selection and request completion.

Spec: `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md` §§2–3 (monotonic durations, one liveness owner, probe/ACK on ordinary ingress); `03-spot-actor/03-mesh-node.ko.md` §7.1; `core/doc/spec/core/socket/README.ko.md` §4 (reciprocal direction selection remains in Core).

Cross-language: C++ `tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:839` advances both bilateral peers; Java `ZLinkJavaRawMeshNode.java:4241` continuously runs its service pump, admission, and liveness. Node's production backend already does the same at `node-raw-mesh-backend.ts:1519`. The structural difference was this manually driven Node fixture, not a missing transport recovery policy.

Classification: **B — fixture progress defect**. Rule count: **2 phase-specific progress rules → 1 complete progress rule**, reused for both request directions.

## Failure 2: Spot overrun timer observation

### Root cause and captured state

The extra 5 ms timer is issued by `DefaultZLinkSpotManager.scheduleIdleSweep` at `runtime/spots/index.ts:912` (original line 908), called by `runIdleSweep`. It belongs to `idle-instance-occupied` from the earlier test `ZLinkSpotManager restores Ready authority when idle eviction loses local occupancy`.

That fixture's quiescence callback set `actorCount = 1` on **every** invocation (`test/contract/spot-manager.test.js:1083`). Its cleanup reset the count to zero, then called `manager.close`, whose quiescence callback set it back to one. Close therefore returned false; the test ignored the return value and left the instance and its idle sweeps active.

The old `withFakeTimerClock` (original line 4185) replaced process-wide `performance.now`, `setTimeout`, and `clearTimeout`. Its `setImmediate` yield let that unrelated idle sweep schedule another 5 ms timer into the fake timer list. The exact test 86 `[10, 5]` failure reproduced on **Spot full-file run 8**: `spot-suite-8.log:513`; issuer stacks are in `spot-suite-8.state`. Existing telemetry was captured in `spot-suite-8.state.flow`.

The additional owner trace (`spot-owner-3.state`) identifies `OccupiedIdleInstanceSpot`, active key `test.mesh\0idle-instance-occupied`, and fake handle `{ delay: 5, cleared: false }`. This same pollution also reproduced a failure in the fractional-tick test 84 (`spot-owner-3.log:501`), confirming that it affected multiple consumers of the global fixture. The runtime was correctly scheduling an idle sweep for an instance the fixture had failed to close; it was not a hidden transport retry.

### Fix and regression

- The occupancy-race fixture introduces occupancy on its first quiescence call only and now asserts that cleanup close succeeds and the instance is absent (`spot-manager.test.js:1081`, `:1098`). Existing race assertions remain.
- `ZLinkTimerClock` extends the existing `OperationClock` with monotonic and UTC reads. The existing managed timer owns the production default; the registry receives and forwards the same clock. An optional internal manager constructor dependency preserves the existing manager/activation integration tests without adding public configuration.
- `withFakeTimerClock` now supplies that clock directly. It does not patch process globals or filter timers by delay/callback. The original overrun assertion remains exactly `[10]` (`spot-manager.test.js:4117`). The wall-clock-jump test also uses the injected UTC clock.
- The new regression at `spot-manager.test.js:3992` runs a real Instance Spot idle sweep alongside an injected Spot timer registry. It requires exactly one managed tick, pending delays exactly `[10]`, unchanged global functions, successful instance cleanup, and no remaining registry timers after disposal.

A temporary in-memory mutation restoring global timer interception makes that regression fail with `[5, 10]`: `timer-regression-old-fixture.log`. This verifies isolation without weakening the timer inventory assertion.

Alternatives considered: fix only the leaked instance cleanup, or fix cleanup and remove process-wide clock interception. The second removes dependence on every other asynchronous owner being idle while this fixture runs.

Ownership: Spot timer owns scheduling and clock reads; the fixture owns its injected scheduler inventory and the instances it creates. Idle-eviction runtime policy is unchanged.

Spec: `framework/doc/framework/common/spec/server/03-spot-actor/10-spot-timer.ko.md` §§1–2 and §8 (cancel, overrun, and tick assertions); transport-liveness §2 / D-095 (monotonic elapsed time, UTC only for timestamps).

Cross-language: .NET `Runtime/Timers/ZLinkTimerScheduler.cs:22` accepts a `TimeProvider`; `tests/Zlink.Framework.UnitTests/Runtime/TimerLifecycleTests.cs:14` injects a manual provider. Node now has the same scoped clock seam. Default scheduling and all three overrun algorithms retain their existing behavior.

Classification: **B — fixture lifecycle and isolation defects**, with internal clock injection authorized by the job's explicit scheduler-injection instruction. Rule count: **production timer clock plus process-wide fixture override (2) → one clock dependency per timer owner (1)**.

## Diff split

| Cause | Files / hunks |
| --- | --- |
| M6A fixture | `framework/languages/node/test/m6a/m6a-runtime.contract.ts`: bilateral test helper and regression; monotonic `pollUntil` |
| Spot fixture | `framework/languages/node/test/contract/spot-manager.test.js`: occupancy cleanup assertion, injected clock fixture and existing timer callers, isolation regression |
| Spot internal clock wiring | `framework/languages/node/packages/framework/src/runtime/spots/spot-timer.ts`, `spot-activation.ts`, `index.ts` |
| Requested report | This file |

The two causes have no shared source hunks. `packages/framework/src/index.ts` continues to export only contracts; `internal.ts` is not exported by package.json. No public contract, Core, binding, other-language implementation, protected documentation, sample, or E2E runner was edited by this job. Concurrent changes in those areas belong to other work and are excluded from this diff split.

## Gate results

| Gate | Result | Evidence |
| --- | --- | --- |
| M6A original + re-admission regression, focused | **300/300 runs**, 2 tests/run | `m6a-fixed.results`, `m6a-fixed-*.log` |
| M6A complete file, final code | **300/300 runs**, 42 tests/run, zero failures | `m6a-final-suite.results`, `m6a-final-suite-*.log` |
| Spot complete file, final code, including original overrun and new isolation regression | **300/300 runs**, 92 tests/run, zero failures | `spot-final-suite.results`, `spot-final-suite-*.log` |
| Prior-fixture mutation checks | Both regressions fail as expected | `m6a-regression-old-fixture.log`, `timer-regression-old-fixture.log` |
| Full `flock /tmp/zlink-samples-gate.lock flock /tmp/zlink-node-gate.lock npm test` | **Exit 1: 1,692/1,692 completed, 1,688 passed, 4 unrelated failures; zero cancelled/skipped**. Build, typecheck, lint, and all sample self-checks passed. | `npm-test.log:8235`, `:12159` |

The full gate's Spot file passed all 92 tests, including the cleanup, isolation, and overrun tests (`npm-test.log:9062`, `:9428`, `:9434`). Its final `&& npm run verify:m6a-runtime` stage was not reached because the runtime gate returned 1; M6A was independently verified in the 300 complete-file runs above. Result-file and TAP audits confirmed all 300 runs completed with the expected case counts, no cancellation, and no skip. `git diff --check` passed for this job's files.

## BLOCKERS

The requested zero-failure full gate is blocked by concurrent sample-runner work:

1. **Configuration scan, 1 failure.** `framework/languages/node/samples/ZoneWorld/test/runner-cleanup.test.js:63` reads `process.env`. `test/contract/e2e-config-file-only-gate.test.js:22` recursively scans the samples tree and excludes only `Runner/` paths and `run-sample.mjs`, so it flags this newly added test file. Evidence: `npm-test.log:3970` (offending path and assertion), `:4061` (file exit 1); `unrelated-config.status` identifies it as untracked and `unrelated-config.source` preserves the offending line. This job did not create or edit it, and the user's scope explicitly reserves ZoneWorld for the other job.
2. **Runner VM fixture, 3 failures.** The concurrent change to `samples/run-sample.mjs` makes `cleanChildren` use `setTimeout` at line 569 and `clearTimeout` at line 572. `test/contract/sample-runner-teardown.test.js:16` evaluates those functions in a VM context that supplies neither global. All three cleanup cases fail with `ReferenceError: clearTimeout is not defined` (`npm-test.log:8270`, `:8286`, `:8319`). This test imports Node built-ins and extracts runner functions; it does not import Framework or the changed Spot timer implementation. The runner source is modified by the other work, while this fixture was not changed by this job.

The full gate ran once and was not retried. No assertions or unrelated files were changed to hide these failures. No remaining failure was found in the two assigned intermittents or their regressions.
