# .NET MeshNode queued Admit loss

**Status: incomplete; the candidate runtime fix is not ready to land.** The requested
intermittent Admit failure is explained and the candidate passes the class 300/300
under load, but the required split gate exposes one related handover liveness
regression. The candidate remains uncommitted for supervisor review. No assertion
was weakened and the remaining failure is not treated as an unrelated baseline.


The failure is **(a), a Framework runtime defect (classification B)**. A late `ConnectionReady` event for the already admitted connection changed the peer epoch before the queued Admit was submitted. The Framework then discarded its own reply as stale. The existing receive waits, deadlines, and assertions remain unchanged. A separate TCP port-allocation race discovered during the required class gate was also removed from this fixture.

- **Owner:** Framework RouteMesh admission owns the logical peer epoch and its liveness state. Core/binding retain physical routing, receive, and completion ownership.
- **Spec:** `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md` §4 (lines 314–339: admission, descriptor idempotency, monitor IDs are diagnostic) and §5 (line 370: preserve the admitted epoch). Core `05-polling.ko.md` §4 distinguishes ordinary receive from REQUEST/WRITABLE completion records; successful SEND produces no completion record.
- **Cross-language comparison:** C++ `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3896` records READY candidates without replacing admitted liveness. Node `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:1594` likewise avoids changing admitted state on delayed READY. The additional epoch/liveness replacement was specific to .NET.
- **Classification:** **B — existing defect**, within the supervisor task's explicit authorization to fix a runtime defect after identifying case (a).
- **Rules before/after:** admitted-peer epoch ownership **2 → 1**: admission plus READY processing → admission alone.

The original failure was reported in `/dev/shm/zlink-cs-server-ready-gate-20260906/unit-main.log:37`, at the first Admit wait, before the repeated Admit/Hello sequence. Reproduction and all diagnostic artifacts are under `/dev/shm/zlink-mesh-admit-20260906/`.

The decisive capture is `diagnostic-focused-load/083.diag.log`, also extracted into `evidence-diagnostic-focused-load-083.log`:

1. The node receives and accepts Hello with lifecycle 7 and descriptor revision 3, and queues an exact Admit with peer connection generation 1.
2. `ready-rotates` records the same connection's delayed READY replacing generation 1 with 2 (diagnostic connection ID 11).
3. The queued control drain starts and records `target-check generation=1 current=False`.
4. No `exact-start`, binding submit, or send exception follows. The reply never reaches Core; there is no wrong-lane delivery or lost binding completion to repair.
5. The Dealer makes 160 nonblocking receive attempts over 2.010 seconds; its public monitor snapshot reports zero pending receive messages and bytes. The node remains admitted until fixture teardown.

The cause was the pre-fix `ZLinkManagedMeshNode.cs:8594` admitted-peer block in `OnSocketMonitorEvent`, which rewrote `ConnectionGeneration` and `Liveness`. The control fence at current `ZLinkManagedMeshNode.cs:10771` correctly rejects a mismatched generation; the incorrect READY mutation created the mismatch. The fixture already uses `Stopwatch`, so this is not the D-095 wall-clock defect or a poller that the test thread failed to drive.

The fix removes READY's epoch/liveness writes and retains candidate observation. Adding another monitor ordering/generation guard was considered and rejected because it would add another owner-dependent rule. No timeout, retry, sleep, skip, route fallback, or assertion relaxation was added. Temporary first-chance exception, receive-queue, and submit-boundary diagnostics were removed.

`QueuedAdmit_SurvivesConnectionReadyDeliveredAfterHello` is the deterministic regression at `MeshNodeShutdownSealTests.cs:157`. It uses the existing socket interfaces and two internal constructor dependencies: a `TaskScheduler` and an `ISocketMonitor` factory. The monitor forwards actual binding events while deferring READY; the scheduler holds the queued Admit until that READY has been handled. A subsequent Draining Update is an ordered witness: if Admit is discarded, the first received record is Update and the assertion fails immediately. The test uses public blocking send/receive and task handshakes, with no clock window, timed polling, or binding reflection. Production defaults retain the existing scheduler and binding monitor.

The regression failed against the old READY behavior in 287 ms with `Expected: Admit; Actual: Update` (`regression-red.trx`), then passed with the fix (`regression-green.trx`). The fixed class also passed 10/10 (`class-green.trx`). These builds used an isolated artifacts directory so the requested pre-fix repetitions continued against unchanged diagnostic binaries.

The first post-fix load campaign stopped at run 103 on a separate fixture failure: `CrossedHelloAdmit` could not bind its second node (`ZlinkBindException`, errno 98/EADDRINUSE). All 103 idempotent-Admit and deterministic regression executions had passed. The complete failed run is retained in `gate-load-admit-only/103.log` and `.trx`; no retry or skip was counted as a pass.

The pre-change `AllocateTcpEndpoint` helper (`MeshNodeShutdownSealTests.cs:620` in that build) bound a temporary `TcpListener`, released it, and later handed the now-unowned port to the node. Initial fixture bindings now use `tcp://127.0.0.1:0` and read `Status().LocalEndpoint` after the actual socket binds. Port selection/reservation ownership therefore changes **2 → 1**: temporary listener plus node socket → node socket alone. The crossed-Hello test asserts that the two held endpoints differ and uses the same gated scheduler to configure both outbound intents before either Hello submits. Restart tests retain the first peer's actual endpoint and reuse it on restart. TCP transport, all prior assertions, and all prior receive/retry/liveness time bounds are retained. The corrected class passed 10/10 in `class-atomic-bind.trx`; a fresh required 300-run load gate follows this fixture change.

All builds and tests source `framework/languages/dotnet/perf/scripts/dotnet-env.sh` and hold `/tmp/zlink-dotnet-gate.lock`. Each load invocation runs exactly 20 owned `yes > /dev/null` processes and joins them before releasing the lock. Package, binding DLL, and native library hashes remained unchanged; `environment.json` and `gate-environment.json` record the artifacts. VSTest diagnostic files preserve the existing `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` and `ZLINK_DEBUG_FRAMEWORK_TASKS=1` output, which the ordinary console logger does not display. RouteMesh control-plane admission is outside application message-flow tracing.

<!-- gate-results-start -->
| Validation | Runs / tests | Result |
|---|---:|---|
| Before fix, class without artificial load | 200 runs / 1,800 tests | 1,797 passed; 3 failed |
| Before fix, class with `yes` x20 | 100 runs / 900 tests | 895 passed; 5 failed |
| Candidate plus atomic fixture bind, class with `yes` x20 | 300/300 runs / 3,000 tests | 3,000 passed; 0 failed; 0 skipped |
| Split unit gate excluding `CanonicalActorJoinIngressReplyTests` | 2,001 tests | 2,001 passed; 0 failed; 0 skipped |
| Complement: `CanonicalActorJoinIngressReplyTests` | 16 tests | 15 passed; 1 failed; 0 skipped |

The final build passed with 0 warnings and 0 errors. Both split commands used
`--blame-hang --blame-hang-timeout 10m`. Every class TRX contains the new deterministic
regression. Final class artifacts are `gate-load/001.trx` through `300.trx`;
split artifacts are `split-gate/unit-main.trx` and `split-gate/unit-join.trx`, below
`/dev/shm/zlink-mesh-admit-20260906/`.

**Remaining failure:**
`CanonicalActorJoinIngressReplyTests.RouteAdmission_HandoverStartsFreshLivenessDeadline`,
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs:732`:
expected admitted count 1, actual 0. The required zero-failure gate is **not achieved**.
<!-- gate-results-end -->

Changed implementation/test files:

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs`

No commit was created. No Core, binding, protected documentation, other-language, performance, or ClientServer runtime file was changed by this job.

## Remaining handover obligation and contract boundary

The failed complement is related to the candidate change. Its fixture admits a
Dealer, waits 13 seconds, hands over to another Dealer with the same RID and the
same descriptor, then checks admission after another 3 seconds. Existing logs in
`handover-focused.diag.log:427` and `:459` show both Hello records accepted with
lifecycle 1 / revision 1. The candidate retains the old 15-second liveness deadline,
so the peer is no longer admitted at the final assertion. The focused run repeats
this exact failure (`handover-focused.trx`); no test code or timing was changed.

The admission matcher returns the already admitted RID immediately
(`ZLinkMeshPeerAdmission.cs:46`), and the guard calls identical descriptor bytes
idempotent (`ZLinkServiceAdmissionGuard.cs:49`). Consequently the liveness creation
in `ZLinkManagedMeshNode.cs:8335` is not reached for this actual physical replacement.
Before the candidate, the unconditional READY mutation was the sole path renewing
that deadline, and it also invalidated queued Admit operations on an ordinary late
READY. Removing that mutation alone satisfies only one of the two obligations.

An isolated comparison restored only the original READY block into a source copy
outside the repository, while retaining the deterministic test seams. It did not
replace the normal gate binaries or repository source. In
`baseline-ready-comparison.trx`, the handover test passes, but the deterministic
late-READY test fails in 281 ms: expected Admit, actual Update. The candidate has the
opposite result. This rules out dismissing the complement failure as unrelated load.
The comparison's MSBuild override and artifacts are preserved in
`baseline-ready.targets` and `baseline-ready-artifacts/`.

The full cross-language comparison also matters: C++ establishes liveness in
`raw_mesh_node_owner.cpp:2903` after candidate-based admission, and Node does so in
`raw-service-mesh-runtime.ts:1317`. Their READY handlers do not directly overwrite an
admitted liveness state. The current .NET admission path consumes only candidate
direction and does not establish the same distinction between an identical Hello
on the existing connection and one on its actual replacement.

However, importing their monitor-derived connection fencing would conflict with
the current contract: service-wire §4 (`06-wire-protocol.ko.md:338`) makes monitor
`connection_id` diagnostic/correlation-only, while §5 (`:370`) requires the epoch to
remain stable on repeated admission and to change on a genuine physical replacement.
The .NET binding contract explicitly has no public pair/generation surface
(`bindings/doc/spec/dotnet/README.ko.md:774`). `Received` exposes RID and an opaque
request reply token (`bindings/dotnet/src/Zlink/Contracts/Messaging/Received.cs:51`);
RouteMesh Hello is a plain SEND. Its `Received.Send()` builder captures the source
RID, not an inspectable physical connection identity
(`bindings/dotnet/src/Zlink/Runtime/Messaging/Received.Operations.cs:9`, `:37`).
This is an unresolved observation/ownership contract boundary, not evidence of a
Dealer completion defect or a reason to add a second Framework transport fence.

Classification: **B** for the demonstrated late-READY epoch mutation; **D** for the
remaining contract decision needed to identify a same-descriptor physical admission
replacement without a forbidden monitor fence. The latter has not been implemented.
The supervisor must resolve that boundary before this candidate can be landed. A
standalone READY deletion must not be committed as the completed fix. No Core,
binding, protected documentation, other language, performance, or ClientServer
runtime change was made to force the gate green.
