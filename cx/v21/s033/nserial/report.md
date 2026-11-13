# Node request-serial round-trip latency (#3872)

## Result

Removed one redundant `await this.yieldIfNeeded()` from the Node application dispatch worker. It ran after each claim, before the worker incremented the shared record budget and performed the budget-controlled fairness check. The latter check remains. The Framework/binding median ratio rose from 0.712 to 0.749 for request-serial 1024, from 0.660 to 0.827 for request-serial 4096, and from 0.604 to 0.718 for request-backpressure 1024. All three target cells improved, so the change is retained.

No queue, flag, counter, public API, or spec was added or changed. The required E5 completion microtask remains one per successful round trip.

## Hop trace

The temporary `async_hooks` tracer warmed each pair with 20 requests, then counted 200 sequential request/reply cycles. It attributed Promise resources by the first Framework or binding runtime stack frame, counted global async resource types, and wrapped the binding readiness notification and Framework readiness handler. The tracer was kept under `/tmp` and removed after reporting. Fractional values below are observed totals divided by 200; polling and callback counts are measurement-window averages.

| Step | File:function | Framework per request (before → after) | Binding per request |
|---|---|---:|---:|
| Operation registration Promise | `operation-registry.ts:OperationRegistry.register` | 1 init → 1 init | 0 |
| Binding completion entry | `completion_owner.js:CompletionEntry` | 4 init, 2 before → unchanged | 4 init, 2 before |
| Binding reply observation Promise | `node-raw-binding-port.ts:observeRequestReply` | 1 init, 1 before → unchanged | 0 |
| Raw backend pump Promise | `node-raw-mesh-backend.ts:ZLinkNodeRawMeshBackend.pump` | 1.665 init → unchanged | 0 |
| Ready wait Promise | `mesh-dispatch-pump.ts:ZLinkMeshDispatchPump.waitForApplicationReady` | 1 init → unchanged | 0 |
| Application worker Promise and continuations | `mesh-dispatch-pump.ts:ZLinkMeshDispatchPump.runApplicationWorker` | 3 init, 1.99 before → 1 init, 0.995 before | 0 |
| Required completion-turn microtask (E5) | `node-raw-mesh-backend.ts:requestObserverFor → Object.operationCompleted` | 1 queued and 1 run → unchanged | 0 |
| Native completion readiness notification | `completion_owner.js:CompletionOwner.notifyReadable` | 2.665 calls → unchanged | 0 calls in raw binding pair |
| Native readiness handler callback | `node-event-loop-poller.ts:ZLinkNodeEventLoopPoller.setReadableHandler` | 1.665 calls → unchanged | 0 calls in raw binding pair |
| `setImmediate` loop callbacks | binding raw receive pump | 0 → unchanged | 1.99 init, 1.985 before |
| `process.nextTick` callbacks | all traced paths | 0 → unchanged | 0 |
| Timer/macrotask | `RequestRuntime.schedule` | 1 timeout was created in the 200-request window; not one per round trip | no per-round-trip timeout observed |

The global Promise totals corroborate the worker result: before the change, Framework had 2,533 Promise inits and 1,198 `before` callbacks, versus binding 1,796 and 997. Afterward, Framework had 2,133 and 999, while binding had 1,800 and 999. The change removes 400 Promise inits and 199 continuation callbacks over the 200 traced requests. There were no Framework `Immediate` or `TickObject` resources in the trace. Binding `Immediate` resources are its raw polling loop, not a Framework-added hop.

## Change decision and spec

| Change considered | Decision | Reason |
|---|---|---|
| Remove the unconditional `yieldIfNeeded()` await after a completed claim | **Keep** | Removed one worker Promise layer/continuation per request; both serial cells and the backpressure check improved. The existing budget increment and conditional fairness yield remain. |
| Remove the completion observer's E5 `queueMicrotask` | **Drop** | §11 requires the caller's new execution turn. The trace confirms exactly one E5 microtask per successful request, so this turn remains. No benchmark change was retained for this option. |
| Temporary source/target CPU pinning in `run_local.sh` | **Drop from source** | Used only to make the comparisons reproducible; the temporary edit was reverted before commit. |

The rule count at `runApplicationWorker` changed from two `yieldIfNeeded()` decision calls per claim to one. The single remaining call still checks the existing shared per-worker fairness budget. No state was added.

- **Owner and spec:** `ZLinkMeshDispatchPump.runApplicationWorker` owns the application worker drain; `08 §4.3 W5` and §4.4 define the drain/fairness behavior. The removed call duplicated the fairness check already performed after the budget increment. `01 §11` and `08 §3 E5` still own the completion turn, which is unchanged.
- **Cross-language comparison:** .NET `ZLinkCompletionDispatcher`, Java `ZLinkServiceCompletionDispatcher`, and C++ provider-task dispatch each post one completion to their language runtime's asynchronous execution mechanism. Node keeps its single E5 turn; this change only removes a redundant application-worker await after the handler terminal.
- **Classification:** B, existing redundant worker await. No public completion or handler-order contract changes.
- **Refactoring review:** performance, POSDDD ownership, and unused/duplicated code were checked on the changed pump and its request dispatch path; one duplicate await was removed, no other findings remained, and none were deferred.

## Four-round A/B measurements

All runs were on 2026-11-06 with `WARMUP_SECONDS=30`, `DURATION_SECONDS=5`, four rounds, and `SKIP_BUILD=1`. Condition B pinning used source CPUs 0–7 and target CPUs 8–15. The runner's pinning change was temporary and reverted. `ratio` is the candidate branch median divided by the same-day binding median. Each run had matching submitted/completed and target-received counts.

Throughput is messages/s. Run arrays preserve measurement order; medians are the middle-pair average. `branch base` is the checked-out branch before this runtime change; `candidate` is the committed implementation; `main` is the prebuilt `ee1139025782688e33109455041d8a0222cc8fe3` reference.

| Cell | Branch base runs (median) | Candidate runs (median) | Main runs (median) | Same-day binding runs (median) | Candidate / binding | Candidate vs branch base |
|---|---:|---:|---:|---:|---:|---:|
| request-serial 1024 | 4878.2, 3451.6, 4334.0, 4682.8 (4508.4) | 3473.4, 4469.8, 5100.0, 5012.6 (4741.2) | 3429.8, 5185.0, 4485.6, 3479.8 (3982.7) | 6712.0, 7718.4, 5952.4, 5287.0 (6332.2) | 0.749 | +5.2% |
| request-serial 4096 | 3204.4, 3220.0, 4584.6, 4436.6 (3828.3) | 4715.4, 4879.0, 4673.6, 5066.4 (4797.2) | 3216.6, 4606.2, 4493.8, 3092.6 (3855.2) | 5863.8, 4984.0, 5744.4, 7447.6 (5804.1) | 0.827 | +25.3% |
| request-backpressure 1024 | 13644.8, 13849.6, 14566.4, 15846.4 (14208.0) | 16896.0, 16537.6, 17433.6, 16870.4 (16883.2) | 12083.2, 12108.8, 12800.0, 14848.0 (12454.4) | 19734.0, 25458.4, 26753.0, 21573.2 (23515.8) | 0.718 | +18.8% |

Median mean and p95 latency (ms):

| Cell | Branch base mean / p95 | Candidate mean / p95 | Main mean / p95 | Binding mean / p95 |
|---|---:|---:|---:|---:|
| request-serial 1024 | 0.21890 / 0.33798 | 0.20845 / 0.31836 | 0.25155 / 0.36157 | 0.15555 / 0.22511 |
| request-serial 4096 | 0.26338 / 0.37143 | 0.20477 / 0.26990 | 0.26217 / 0.36522 | 0.16845 / 0.24626 |
| request-backpressure 1024 | 625.489 / 786.152 | 538.292 / 578.828 | 710.352 / 794.927 | 1.399 / 7.515 |

## Verification and commit

- Node build: passed.
- Focused completion-path and Promise-cost contracts: 15/15 passed.
- Full Node suite: passed; runtime gate announced and completed 4,342 tests, and M6A runtime contract passed 48/48.
- `flock -s /root/jobs/fx/cx/measure.lock bash scripts/format/format.sh --check node`: passed.
- `git diff --check`: passed; final worktree is clean.
- Commit: `2d04eaae0eb9b1b89581a249616c600dd30d2b7e` (`node: remove redundant dispatch worker await`). The preceding test-only commit `8d0ce1278f` remains in history. Nothing was pushed.
