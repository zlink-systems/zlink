# Framework Implementation Gap Record

[Spec index](README.en.md) · [RouteMesh topology](07-channel-topology.en.md) ·
[Internal target selection and route cache](../internals/06-routing-and-cache.en.md)

This document is not the formal public contract. It records differences found by comparing
the common spec with the current implementation, and the evidence that closes those
differences. The formal contract is owned by the common spec and each language's exact
interface; this document alone does not mark another feature complete.

## 1. Review scope

This record covers the JVM Java/Kotlin Framework's Location Store-backed object-peer
connection and the TicTacToe sample execution path. A manual endpoint is connection intent;
the lifecycle generation and security identity supplied by a descriptor must be carried as
the runtime admission fence.

## 2. Closed implementation difference

| Item | Target | Previous implementation | Result | Status |
|---|---|---|---|---|
| `JVM-TOPO-001` | The peer handshake in `07-channel-topology` and the Java/Kotlin runtime | `connectManualObjectPeers` and `ensureManualObjectPeer` passed only the endpoint and RID, so they used lifecycle generation `0` and an RID-based security-identity fallback. | Both owner-layer paths pass the descriptor's endpoint, RID, lifecycle generation, and security identity to extended `connectPeer`. | closed |

The change does not add fence values to sample call sites. The application keeps using the
existing public manual-endpoint API, while the runtime owns the match between the descriptor
and the transport.

## 3. Verification evidence

- The Java raw-binding regression confirms that a peer intent with the wrong lifecycle
  generation/security identity is not admitted, and that reconnecting with the descriptor
  values is admitted.
- The Java raw regression passed with:
  `cd framework/languages/java && ./gradlew --no-daemon --no-parallel :zlink-framework-core:test --tests '*ZLinkJavaRawMeshNodeM6ATest' --tests '*ZLinkActorCreationCoordinatorTargetSelectionTest'`.
- The Java aggregate passed with the following command. After the preflight gates, the real
  client/server self-checks for TicTacToe, Bingo, DeliveryDispatch, GameQuest, ShoppingMall,
  and SupportChat completed, including `PASS TicTacToe.Java` and the final success marker.
  `cd framework/languages/java/samples && ZLINK_SAMPLE_LANGUAGES=java timeout 1800s ./run_samples.sh`
- After the Java aggregate completed, the Kotlin aggregate passed with the following command.
  `PASS TicTacToe.Kotlin` and the six Kotlin real-process self-checks, including Bingo,
  DeliveryDispatch, GameQuest, ShoppingMall, and SupportChat, completed.
  `cd framework/languages/java/samples && ZLINK_SAMPLE_LANGUAGES=kotlin timeout 1800s ./run_samples.sh`
- Two additional standalone Kotlin Bingo repetitions also exited with `0`. The sample now starts
  the `waitFor` operations that must be registered before an event with
  `CoroutineStart.UNDISPATCHED`.

## 4. Runtime shutdown and the sample-runner boundary

The common shutdown spec applies the public 30-second drain deadline first and allows bounded
teardown after that deadline. The current JVM host runtime follows that order for resource
cleanup, so a sample runner that force-stops a process at 30 seconds can confuse sample results
with runtime teardown results. The common runner and the Java `DeliveryDispatch` runner now
observe this bounded cleanup for up to 90 seconds before using `SIGKILL`. A forced kill or cleanup
failure is returned as non-zero instead of being hidden as a successful sample. The 90 seconds is
the sample runner's observation limit; it is not a change to the Framework public shutdown
deadline.

## 5. Boundary of this decision

Closing `JVM-TOPO-001` in this record is limited to the runtime paths and Java/Kotlin sample
execution covered here. It does not change the status of the Framework-wide contract,
package provenance, clean-consumer, common cross-language E2E, or performance gates.

## 6. Node.js 0.10.0 Review

This section records the `0.10.0` review of the Node.js/NestJS Framework and Node samples.
Node runtime behavior follows the common internals and common formal spec; the Node exact
interface owns the precise public-interface representation.

### 6.1 Closed implementation differences

| Item | Target | Previous implementation | Result | Status |
|---|---|---|---|---|
| `NODE-ROUTE-001` | `16 Spot Address Messaging`, `18 Spot and Actor Routing`, Node `requestToSpotAddress` | An existing Ready Instance route could refresh its route and resubmit the same request after returning `ActorLocationStale`. | Only route absence reported as `RequestTargetNotFound` before admission is reconsidered as an Instance-intent cold-activation route. An `ActorLocationStale` result may have crossed the transport boundary, so it is returned as a terminal error and the same operation is not hidden-retried. | closed |
| `NODE-RELOC-001` | Entry Spot Actor relocation membership restore | The M6C production-target fixture did not implement the actual `ZLinkActorRuntimeState.clearJoinedSpot()` contract, so Entry membership restoration ended in `TypeError`. | The fixture implements the actual state contract and verifies the runtime path that clears joined-Spot state when restoring Entry Spot membership. | closed |
| `NODE-SAMPLE-001` | Node sample README and the Bingo routing-id static gate | The sample README named a Framework version other than `0.10.0`, and the Bingo verifier incorrectly expected a literal marker even though the runner uses a parameterized default marker. | Node sample metadata is unified at `0.10.0`, and the verifier checks the runner's actual marker declaration and use. | closed |

### 6.2 Verification evidence

- `npm ci` completed with the fresh local package and updated provenance. The Node package and
  Framework package versions are `0.10.0`.
- `npm run typecheck` and `npm run lint` passed.
- `npm run verify:m5-foundation`: 5/5.
- `npm run verify:m6a-runtime`: 35/35.
- `npm run verify:m6b-runtime`: 84/84.
- `npm run verify:m6c-runtime`: 79/79.
- The real-process runner `npm run verify:samples` passed all seven samples: TicTacToe.Ts,
  Bingo.Ts, DeliveryDispatch.Ts, SupportChat.Ts, GameQuest.Ts, ShoppingMall.Ts, and ZoneWorld.
- The Node sample static contract tests passed 35/35 when the process aggregate test was
  excluded.

### 6.3 Remaining verification conditions

- The full `test/contract/channel-client.test.js` aborts after its first 27 tests in the public
  dealer/router binding-socket test with `Invalid argument` at
  `core/src/runtime/utils/mutex.hpp:108`. The target test passes in isolation and also passes
  with the selected preceding tests, so the current evidence points to a Core native mutex
  lifecycle or test-teardown issue rather than a Node public-routing defect. This item is not
  marked as a closed Node implementation difference.
- The test that reruns the process aggregate inside `sample-regression` failed once on a
  ZoneWorld log-marker timeout. The independent `npm run verify:samples` passed all seven samples
  in the same changed worktree, so this remains a process-runner repeatability condition rather
  than a sample contract difference.
- Common cross-language process E2E, clean-consumer, full multi-language package provenance, and
  performance gates are not marked complete by this Node review.
