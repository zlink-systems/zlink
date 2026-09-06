# .NET RouteMesh admission candidate implementation

**Status: complete.** D-132 is implemented without changing the public API,
the control fence, timeout/retry policy, poller ownership, or Core/binding code.
All requested gates pass with zero failures and zero skipped tests.

## Result

`ConnectionReady` now records only a pending physical candidate. A successful
Hello admission consumes that candidate. When an admitted peer already exists,
that consumption is the single replacement decision: the node issues a new
connection generation and creates fresh liveness from the admission timestamp.
A same-descriptor Hello with no pending candidate remains idempotent and replies
with Admit without changing the connection epoch or liveness deadline.

- **Owner:** Framework RouteMesh admission owns the logical peer connection
  generation and liveness transition. `ZLinkMeshConnectionCandidates` owns only
  whether READY evidence is pending or already consumed. Core/binding continue
  to own physical routing, receive, and completion.
- **Spec:** `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md`
  §4 lines 317–341 and §5 lines 359–370: monitor `connection_id` is diagnostic,
  repeated admission preserves the admitted epoch, and genuine physical
  replacement creates a new epoch with fresh liveness.
- **Cross-language comparison:** C++ records READY candidates in
  `raw_mesh_node_owner.cpp:3896-3929` and establishes liveness after candidate
  admission at `:2902-2905`. Node performs the corresponding admission-owned
  liveness transition in `raw-service-mesh-runtime.ts:1306-1322`; its monitor
  observer at `:1594-1609` does not overwrite admitted liveness. .NET now has
  the same ownership rule while retaining its binding-specific candidate
  registry because routed receive does not expose a physical connection ID.
- **Classification:** **B — existing Framework runtime defect**, as approved by
  D-132.
- **Rules before/after:** admitted-peer epoch ownership **2 → 1**: READY plus
  admission → admission alone. Candidate pending/consumed state has one owner,
  `ZLinkMeshConnectionCandidates`.

The two considered alternatives were rejected. Restoring READY mutation renews
liveness for a replacement but invalidates an already queued Admit on an
ordinary late READY. Comparing monitor `connection_id` with inbound frames would
add a second transport fence and conflicts with the diagnostic-only contract.
Candidate consumption keeps the decision inside admission and needs neither
alternative.

## Changes

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8105-8345`
  selects the pending handshake candidate, treats a candidate-consuming Hello
  for an admitted RID as replacement, rotates the connection generation, and
  lets the existing liveness creation path establish the fresh deadline.
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8599-8627`
  keeps READY observation-only. The existing control target fence at
  `:10793-10796` is unchanged.
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshPeerAdmission.cs:210-246`
  keeps admitted physical candidates for disconnect tracking but excludes
  consumed candidates from later handshakes.
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs:157-262`
  adds the deterministic no-candidate same-connection complement and retains
  `QueuedAdmit_SurvivesConnectionReadyDeliveredAfterHello`. The monitor interface
  fixes READY-before-Hello or READY-after-Hello ordering without clock windows,
  sleeps, reflection, or binding-private access.
- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ZLinkMeshPeerAdmissionTests.cs:203-227`
  verifies that one admission consumes a pending candidate exactly once.
- The starting candidate's atomic `tcp://127.0.0.1:0` fixture binding remains;
  port reservation ownership stays **2 → 1** (temporary listener plus node →
  node socket alone).

No protected specification document, Core, binding, other language, performance,
ClientServer runtime, or Streams file was changed by this job.

## Validation

All .NET commands sourced
`framework/languages/dotnet/perf/scripts/dotnet-env.sh`. The unit and regression
commands held `/tmp/zlink-dotnet-gate.lock`; samples held
`/tmp/zlink-samples-gate.lock` and then `/tmp/zlink-dotnet-gate.lock`. Before the
split gate, `/proc/loadavg` was `9.00` and `pgrep -c lto1` was `0`.

| Validation | Result | Artifact |
|---|---:|---|
| Focused new/affected tests | 4/4 passed | console result |
| `MeshNodeShutdownSealTests` with `yes` ×20 | 300/300 runs; 3,300 passed, 0 failed, 0 skipped | `/dev/shm/zlink-dotnet-d132-class-znlyqg/` |
| Split unit gate excluding `CanonicalActorJoinIngressReplyTests` | 2,003 passed, 0 failed, 0 skipped | `/dev/shm/zlink-dotnet-d132-split-sLoaLE/unit-main.trx` |
| `CanonicalActorJoinIngressReplyTests` complement | 16 passed, 0 failed, 0 skipped | `/dev/shm/zlink-dotnet-d132-split-sLoaLE/unit-complement.trx` |
| `Zlink.Framework.SampleRegressionTests` | 157 passed, 0 failed, 0 skipped | `/dev/shm/zlink-dotnet-d132-sample-regression-5drN8V/sample-regression.trx` |
| .NET samples | 7/7 passed | `/dev/shm/zlink-dotnet-d132-samples-rTNdOv/aggregate.log` |

The unchanged `RouteAdmission_HandoverStartsFreshLivenessDeadline` passed in the
focused run and in the 16-test complement. The sample completion markers were
present for TicTacToe, Bingo, SupportChat, ShoppingMall, DeliveryDispatch,
GameQuest, and ZoneWorld.

The build emitted the pre-existing nullable warning CS8619 at
`Runtime/Spots/ZLinkSpotNodeCatalog.cs:768`; it produced no build error and no
test failure. Package inputs were unchanged:

- `Systems.Zlink.0.17.0.nupkg` SHA-256:
  `350b8b789a1b31328bd477d895283efc6b986a1b242c310eadda914cf79c98c3`
- `core/build-dev/lib/libzlink.so.0.17.0` SHA-256:
  `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`

## Remaining failures

None. No commit was created.
