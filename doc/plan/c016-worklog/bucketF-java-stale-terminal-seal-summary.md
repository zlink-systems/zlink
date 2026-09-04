# Bucket F — Java stale-terminal relocation seal

## Result

The failure is a test-harness ordering race, not a binding refusal and not a
Framework relocation sequencing defect. `ActorManager.create(...).submit()`
completes with the create application result; it does not promise that the
physical Actor queue entry has already released its gate. The test immediately
called the runtime-internal non-blocking `trySealActorRelocation`, which is
allowed to return empty while that entry is still active.

The test harness now waits for the admitted creation turn to reach its drain
barrier before it creates the synthetic post-cut state. No production runtime,
binding, Core, protected spec, or sample code changed.

## Determinism

The unmodified focused class did not reproduce the gate failure in five runs.
Together with the prior full-gate failure, this classifies the symptom as a
scheduling race rather than a deterministic D-B85 behavior change.

| Build / run | Scope | Result |
| --- | --- | --- |
| pre-D-B85 gate | full Java core suite | target test passed |
| D-B85 gate v2 | full Java core suite | target test failed at the absent seal |
| pre-fix 1 | `*EntrySpotActorDispatchTests*` (10 tests) | pass |
| pre-fix 2 | same | pass |
| pre-fix 3 | same | pass |
| pre-fix 4 | same | pass |
| pre-fix 5 | same | pass |
| post-fix 1 | same | pass |
| post-fix 2 | same | pass |
| post-fix 3 | same | pass |
| post-fix 4 | same | pass |
| post-fix 5 | same | pass |

Pre-fix logs are under
`zlink-work/c016/logs/bucketF-java-stale-terminal/pre-fix-run-*.log`;
post-fix logs are under
`zlink-work/c016/logs/bucketF-java-stale-terminal/post-fix-run-*.log`.

## Transition trace

With `ZLINK_JAVA_STREAM_TRACE=1`, the successful isolated transition was:

1. `actor-create notify-start actor=actor-a`
2. `actor-dispatch turn-start actor=actor-a`
3. `actor-dispatch run-turn-operation-return actor=actor-a done=false`
4. `actor-dispatch run-turn-join-finish actor=actor-a error=none`
5. `actor-dispatch run-turn-handler-finish actor=actor-a error=none`
6. `actor-dispatch turn-complete actor=actor-a error=none`
7. `actor-create notify-complete actor=actor-a accepted=true`
8. `actor-create activate-complete actor=actor-a error=none`

The full trace is
`zlink-work/c016/logs/bucketF-java-stale-terminal/message-flow-normal.log`.
Framework message-flow was temporarily raised to `NORMAL` for this capture and
the diagnostic edit was removed afterward. It emitted no `zlink.message_flow`
record for this interval: Actor creation lifecycle and relocation sealing are
control-plane work, while Spec 26 message-flow covers application dispatch.
The existing Java stream trace therefore supplies the relevant transition
evidence.

There is no relocation request in this test before the failing line. There is
also no ROUTER request or `NOT_CONNECTED` result: the test uses the fake backend
and locally creates the Actor. The D-B85 binding request path is therefore not
executed by this test; the rebuilt gate exposed an existing scheduling race
rather than changing this test's semantics.

## Cause and owner

Owner: **test harness**.

- `ZLinkActorRuntime.java:1201-1209` runs the create notification through the
  Actor dispatch lane and builds the create result from that stage.
- `ZLinkSerialExecutionQueue.java:1097-1104` completes the operation result
  before completing the Actor gate.
- Queue cleanup later clears `active` in
  `ZLinkSerialExecutionQueue.java:788-813`.
- During that intentional interval,
  `ZLinkSerialExecutionQueue.java:872-891` returns an empty relocation seal to
  a non-current caller because `active != null`.
- The failing harness used `create.submit().get()` followed immediately by
  `trySealActorRelocation(...).orElseThrow()` (formerly
  `EntrySpotActorDispatchTests.java:522-528`) without a relocation lifecycle
  boundary or a drain barrier.

Changing the serial queue to publish operation results only after physical
cleanup would broaden the completion contract for all queues and is not
required by the relocation contract. Retrying `trySeal` would hide the missing
harness boundary. The minimal correct choice is to establish the boundary the
test needs explicitly.

## Spec match

- `01-execution/01-submit-and-completion.ko.md:44-48`: a normal async create
  terminal waits for the create application result; it does not define queue
  quiescence as part of that result.
- `01-execution/02-handler-turn-and-execution-gate.ko.md:83-95`: an Entry Actor
  uses its Actor gate and does not overlap Actor turns.
- `05-location-relocation/04-relocation-flow.ko.md:108-114`: source relocation
  ends the currently running handler before sealing/capturing queued work.
- `03-spot-actor/08-routing.ko.md:222-234`: work accepted before the seal belongs
  to the old queue/journal; post-seal work belongs to ingress hold and the
  target temporary-queue sequence.

The changed harness follows those boundaries; no assertion or public contract
was weakened.

## Fix and regression coverage

`EntrySpotActorDispatchTests.java:483,528,566` now calls the shared
`awaitActorCreationTurn` helper before synthetic sealing. The helper at
`EntrySpotActorDispatchTests.java:900-904` waits on
`ZLinkActorRuntime.awaitDrainBarrier()` with the existing five-second test
deadline.

The reported regression
`postCutActorArrivalWithoutForwardKeepsTheStaleTerminal` still asserts that a
refused relocation forward is attempted exactly once and does not replay the
turn on the stale source. The two adjacent post-cut regressions use the same
boundary so the identical race cannot migrate to their direct seal setup.

## Gate

| Verification | Result |
| --- | --- |
| focused class, post-fix 5 times | 5/5 commands passed; 50/50 tests passed |
| full `:zlink-framework-core:test` once | 1,207 passed / 2 failed (1,209 total) |

Full-gate log:
`zlink-work/c016/logs/bucketF-java-stale-terminal/post-fix-full-core-test.log`.
The Bucket F test passed in the full suite.

## BLOCKERS

- No Bucket F blocker remains.
- The full core gate is still red only for the two pre-existing Bucket C
  failures:
  - `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
    at assertion line 1400.
  - `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent` at
    assertion line 1439.
