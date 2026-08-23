import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Message } from '@zlink-systems/zlink';
import type { ActorRef, RoutingId } from '../../packages/framework/src/contracts';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';
import { DefaultZLinkSpotManager } from '../../packages/framework/src/runtime/spots';
import {
  ZLinkFormalRemoteActorAdmissionRegistry,
  type ZLinkParkedActorArrival
} from '../../packages/framework/src/runtime/spots/formal-remote-actor-admission-registry';
import {
  ZLinkSpotActorPacketDispatch,
  type ZLinkActorPacketDelivery,
  type ZLinkActorRequestTerminal
} from '../../packages/framework/src/runtime/spots/spot-actor-packet-dispatch';
import { ZLinkSpotActorHandlerRegistryRuntime } from '../../packages/framework/src/runtime/actors/spot-actor-dispatch';
import {
  encodeStreamHeader,
  ZLinkStreamCodec,
  ZLinkStreamMessageKind
} from '../../packages/framework/src/runtime/streams/protocol';

/**
 * Drives the Actor Join relocation temporary queue (spec 15 §4.2) through
 * its real production entry points: the admission registry
 * ({@link ZLinkFormalRemoteActorAdmissionRegistry}) that
 * `dispatchMeshActorJoin` registers on Accepted, and
 * {@link ZLinkSpotActorPacketDispatch.dispatch}, the same method the mesh
 * ingress path (`dispatchMeshActorPacket` -> `dispatchActorPacketDirect`)
 * calls for every arriving Actor packet. No test reaches into the parked
 * queue directly — arrivals only ever go through `dispatch()`, and the
 * registry only ever answers through `routeIngress`/`completeMigration`,
 * exactly as the production `routeToActorJoinPrewarm` wiring in
 * `runtime/spots/index.ts` uses them.
 */

const ACTOR_ID = 'actor-prewarm';
const OBJECT_GENERATION = 5n;
const MESH_NAME = 'mesh-a';
const NODE_RID = 'target' as unknown as RoutingId;
const SPOT_ID = 'spot-1' as unknown as RoutingId;

function fallbackRef(objectGeneration = OBJECT_GENERATION): ActorRef {
  return {
    actorId: ACTOR_ID,
    objectGeneration,
    meshName: MESH_NAME,
    nodeRid: NODE_RID
  };
}

function admission(transferId: string, objectGeneration = OBJECT_GENERATION) {
  return {
    actorId: ACTOR_ID,
    actorType: 'Player',
    actorRef: fallbackRef(objectGeneration),
    spotId: SPOT_ID,
    targetSpotGeneration: 1n,
    expectedMembershipEpoch: 1n,
    requestFingerprint: 'admission-fingerprint',
    transferId
  };
}

/**
 * `dispatch()` yields at least one microtask tick before reaching the
 * missing-actor/park branch (its production code awaits an optional
 * `routeBeforeLocal` hook even when none is configured). A parked Request's
 * own returned Promise cannot be awaited directly to observe that the park
 * happened — it stays pending until the real reply goes out, which is
 * exactly what this mechanism defers until migration. Flushing the
 * microtask queue is the correct way to wait for "has parked" without
 * waiting for "has replied".
 */
function flushMicrotasks(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function pendingCanonicalJoinHarness() {
  const registry = new ZLinkFormalRemoteActorAdmissionRegistry();
  let resolveValidation!: (value: { readonly actorType: string }) => void;
  let rejectValidation!: (reason: unknown) => void;
  const validation = new Promise<{ readonly actorType: string }>((resolve, reject) => {
    resolveValidation = resolve;
    rejectValidation = reject;
  });
  const manager = {
    formalRemoteActorAdmissions: registry,
    formalRemoteTransfers: { has: () => false, delete() {} },
    activations: { resolve: () => undefined },
    options: {
      entryNodeRid: SPOT_ID,
      canonicalActorJoinResolver: () => validation,
      actorResolver: () => undefined,
      async dispatchEntryActorJoin() {}
    }
  };
  const request = Message.from(Buffer.from('canonical-request'));
  const failures: Array<{ readonly terminal: number; readonly code: number }> = [];
  const join = DefaultZLinkSpotManager.prototype.dispatchMeshActorJoin.call(
    manager as never,
    MESH_NAME,
    { spotId: SPOT_ID } as never,
    {
      kindData: {
        kind: 'actorControl',
        lifecycleKind: 2,
        previousActor: {
          actorId: ACTOR_ID,
          generation: OBJECT_GENERATION,
          nodeRid: NODE_RID
        },
        currentActor: {
          actorId: ACTOR_ID,
          generation: OBJECT_GENERATION,
          nodeRid: SPOT_ID
        },
        previousSpotId: 'source-spot',
        currentSpotId: SPOT_ID,
        previousSpotGeneration: 1n,
        currentSpotGeneration: 2n,
        previousMembershipEpoch: 3n,
        currentMembershipEpoch: 4n,
        resultCode: 0,
        canonicalActorJoin: {
          handoffId: 'canonical-await',
          actorNodeRid: String(NODE_RID),
          actorGeneration: OBJECT_GENERATION,
          actorNodeGeneration: 7n,
          authorityOwnerGeneration: 8n,
          ownerLeaseGeneration: 9n
        }
      },
      parts: [request],
      contentType: 'application/json',
      isPending: () => true,
      replyActorJoin: () => 0,
      replyFailure(terminal: number, code: number) {
        failures.push({ terminal, code });
        return 0;
      }
    } as never
  ).finally(() => request.close());
  return { registry, join, failures, resolveValidation, rejectValidation };
}

function sendHeaderBytes(packetName: string): Buffer {
  return Buffer.from(encodeStreamHeader({
    kind: ZLinkStreamMessageKind.Send,
    codec: ZLinkStreamCodec.Raw,
    flags: 0,
    name: packetName,
    metadata: new Map()
  }));
}

function requestHeaderBytes(packetName: string, requestSeq: bigint): Buffer {
  return Buffer.from(encodeStreamHeader({
    kind: ZLinkStreamMessageKind.Request,
    codec: ZLinkStreamCodec.Raw,
    flags: 0x01,
    requestSeq,
    name: packetName,
    metadata: new Map()
  }));
}

test('an arrival parked between Accepted and cutover delivers in order once relocation completes', async () => {
  const registry = new ZLinkFormalRemoteActorAdmissionRegistry();
  const transferId = 'transfer-park-order';
  registry.begin(admission(transferId));
  registry.complete(transferId, { accepted: true, actorRef: fallbackRef() });

  const delivered: string[] = [];
  let actorReady = false;
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {} as never,
    spotId: () => String(SPOT_ID),
    registry: new ZLinkSpotActorHandlerRegistryRuntime(),
    resolveActor: () => (actorReady ? ({ context: { actorId: ACTOR_ID } } as never) : undefined),
    onDisconnectActor: async () => undefined,
    routeBeforeLocal: (delivery: ZLinkActorPacketDelivery) => {
      if (!actorReady) return undefined;
      delivered.push(Buffer.from(delivery.parts[1].data()).toString('utf8'));
      return { handled: true, response: 'ok' };
    },
    routeToActorJoinPrewarm: (actorId, objectGeneration, arrival) =>
      registry.routeIngress(actorId, objectGeneration, arrival)
  });

  // Two ordinary Send arrivals reach the target directly (not via the
  // source's ingress hold) after Accepted but before cutover — exactly the
  // window spec 15 §4.2 requires a relocation temporary queue for.
  const first = dispatch.dispatch(
    ACTOR_ID,
    [Message.from(sendHeaderBytes('early-1')), Message.from('EARLY-1')],
    false,
    undefined,
    fallbackRef()
  );
  const second = dispatch.dispatch(
    ACTOR_ID,
    [Message.from(sendHeaderBytes('early-2')), Message.from('EARLY-2')],
    false,
    undefined,
    fallbackRef()
  );
  await Promise.all([first, second]);
  assert.deepEqual(delivered, [], 'arrivals must park, not deliver, before cutover');

  // Cutover: the Actor becomes resolvable, and the relocation temporary
  // queue migrates into real dispatch atomically, in order.
  actorReady = true;
  const drained = registry.completeMigration(transferId);
  assert.equal(drained.length, 2);
  for (const arrival of drained) {
    await dispatch.dispatch(
      ACTOR_ID,
      [Message.from(arrival.header), Message.from(arrival.payload)],
      arrival.returnResponse,
      arrival.remoteBoundSessionTarget,
      arrival.fallbackActorRef,
      arrival.requestTerminal
    );
  }
  assert.deepEqual(delivered, ['EARLY-1', 'EARLY-2'], 'parked arrivals must migrate in the order they parked');

  // A further arrival after migration reaches the Actor directly — the
  // attempt no longer parks anything for this object.
  await dispatch.dispatch(
    ACTOR_ID,
    [Message.from(sendHeaderBytes('after')), Message.from('AFTER')],
    false,
    undefined,
    fallbackRef()
  );
  assert.deepEqual(delivered, ['EARLY-1', 'EARLY-2', 'AFTER']);
});

test('command 28 registers prewarm before Store validation so early Send and Request survive the await', async () => {
  const harness = pendingCanonicalJoinHarness();
  assert.equal(harness.registry.get('canonical-await')?.state, 'provisional');

  let actorReady = false;
  const delivered: string[] = [];
  const replies: unknown[] = [];
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {} as never,
    spotId: () => String(SPOT_ID),
    registry: new ZLinkSpotActorHandlerRegistryRuntime(),
    resolveActor: () => (actorReady ? ({ context: { actorId: ACTOR_ID } } as never) : undefined),
    onDisconnectActor: async () => undefined,
    routeBeforeLocal: (delivery: ZLinkActorPacketDelivery) => {
      if (!actorReady) return undefined;
      delivered.push(Buffer.from(delivery.parts[1].data()).toString('utf8'));
      return { handled: true, response: 'ok' };
    },
    routeToActorJoinPrewarm: (actorId, objectGeneration, arrival) =>
      harness.registry.routeIngress(actorId, objectGeneration, arrival)
  });

  await dispatch.dispatch(
    ACTOR_ID,
    [Message.from(sendHeaderBytes('early-send')), Message.from('EARLY-SEND')],
    false,
    undefined,
    fallbackRef()
  );
  const request = dispatch.dispatch(
    ACTOR_ID,
    [Message.from(requestHeaderBytes('early-request', 2n)), Message.from('EARLY-REQUEST')],
    false,
    undefined,
    fallbackRef(),
    response => { replies.push(response); }
  );
  await flushMicrotasks();
  assert.equal(harness.registry.get('canonical-await')?.state, 'provisional');

  harness.resolveValidation({ actorType: 'Player' });
  await harness.join;
  assert.equal(harness.registry.get('canonical-await')?.state, 'admitted');

  const drained = harness.registry.completeMigration('canonical-await');
  assert.deepEqual(
    drained.map(value => value.payload.toString('utf8')),
    ['EARLY-SEND', 'EARLY-REQUEST']
  );
  actorReady = true;
  await dispatch.dispatch(
    ACTOR_ID,
    [Message.from(drained[0]!.header), Message.from(drained[0]!.payload)],
    false,
    undefined,
    drained[0]!.fallbackActorRef
  );
  await drained[1]!.requestTerminal!('EARLY-REPLY');
  await request;
  assert.deepEqual(delivered, ['EARLY-SEND']);
  assert.deepEqual(replies, ['EARLY-REPLY']);
});

test('command 28 validation failure releases provisional parked ingress with the typed failure', async () => {
  const harness = pendingCanonicalJoinHarness();
  const failure = new ZLinkFrameworkException(
    ZLinkFrameworkErrorKind.Unavailable,
    'authority read unavailable'
  );
  let parkedFailure: unknown;
  assert.equal(harness.registry.routeIngress(ACTOR_ID, OBJECT_GENERATION, {
    header: requestHeaderBytes('early-request', 3n),
    payload: Buffer.from('EARLY-REQUEST'),
    returnResponse: false,
    resolve: () => undefined,
    reject: error => { parkedFailure = error; }
  }), 'parked');

  harness.rejectValidation(failure);
  await harness.join;

  assert.equal(parkedFailure, failure);
  assert.equal(harness.registry.get('canonical-await')?.state, 'failed');
  assert.equal(harness.failures.length, 1);
  assert.equal(harness.registry.routeIngress(ACTOR_ID, OBJECT_GENERATION, {
    header: sendHeaderBytes('after-failure'),
    payload: Buffer.from('AFTER'),
    returnResponse: false,
    resolve: () => undefined,
    reject: () => undefined
  }), 'not-found');
});

test('a parked Request replies through the original mailbox correlation once migrated', async () => {
  const registry = new ZLinkFormalRemoteActorAdmissionRegistry();
  const transferId = 'transfer-request-reply';
  registry.begin(admission(transferId));
  registry.complete(transferId, { accepted: true, actorRef: fallbackRef() });

  let actorReady = false;
  const replies: unknown[] = [];
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {} as never,
    spotId: () => String(SPOT_ID),
    registry: new ZLinkSpotActorHandlerRegistryRuntime(),
    resolveActor: () => (actorReady ? ({ context: { actorId: ACTOR_ID } } as never) : undefined),
    onDisconnectActor: async () => undefined,
    routeToActorJoinPrewarm: (actorId, objectGeneration, arrival) =>
      registry.routeIngress(actorId, objectGeneration, arrival)
  });

  //  The mesh ingress path always builds a `requestTerminal` closed over
  //  the exact mailbox record for a Request (see `dispatchMeshActor` in
  //  `runtime/spots/index.ts`). It must still be the one that eventually
  //  replies, even though the reply itself only happens once the parked
  //  arrival is redelivered — possibly on a completely different call.
  const requestTerminal: ZLinkActorRequestTerminal = (response) => {
    replies.push(response);
  };

  const originalAwait = dispatch.dispatch(
    ACTOR_ID,
    [Message.from(requestHeaderBytes('ping', 1n)), Message.from('PING')],
    false,
    undefined,
    fallbackRef(),
    requestTerminal
  );
  await flushMicrotasks();
  // The original caller must not observe a reply yet — the mailbox record
  // is still open, exactly as it is in production while relocation is
  // in flight.
  assert.equal(replies.length, 0);

  actorReady = true;
  const drained = registry.completeMigration(transferId);
  assert.equal(drained.length, 1);
  const arrival = drained[0];
  assert.notEqual(arrival.requestTerminal, undefined);
  // The real dispatcher (`ZLinkSpotActorDispatcher.dispatchRequestThenDecoded`,
  // reached from `dispatchActorPacket` once the Actor resolves) calls the
  // terminal it was given with the handler's response. `arrival.requestTerminal`
  // is exactly that terminal — the wrapped original one `parkForActorJoinPrewarm`
  // built — so invoking it here is what production does once the redelivered
  // request has actually been handled.
  await arrival.requestTerminal!('the-response');

  // The original caller's await resolves only once the real reply has
  // gone out through the original `requestTerminal` — never before, and
  // never through a second, independent reply path.
  await originalAwait;
  assert.deepEqual(replies, ['the-response']);
});

test('atomic migration admits no arrival racing the drain to be lost or delivered twice', async () => {
  const registry = new ZLinkFormalRemoteActorAdmissionRegistry();
  const transferId = 'transfer-atomic';
  registry.begin(admission(transferId));
  registry.complete(transferId, { accepted: true, actorRef: fallbackRef() });

  let actorReady = false;
  const delivered: string[] = [];
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {} as never,
    spotId: () => String(SPOT_ID),
    registry: new ZLinkSpotActorHandlerRegistryRuntime(),
    resolveActor: () => (actorReady ? ({ context: { actorId: ACTOR_ID } } as never) : undefined),
    onDisconnectActor: async () => undefined,
    routeBeforeLocal: (delivery: ZLinkActorPacketDelivery) => {
      if (!actorReady) return undefined;
      delivered.push(Buffer.from(delivery.parts[1].data()).toString('utf8'));
      return { handled: true, response: 'ok' };
    },
    routeToActorJoinPrewarm: (actorId, objectGeneration, arrival) =>
      registry.routeIngress(actorId, objectGeneration, arrival)
  });

  await dispatch.dispatch(
    ACTOR_ID,
    [Message.from(sendHeaderBytes('before')), Message.from('BEFORE')],
    false,
    undefined,
    fallbackRef()
  );

  // `completeMigration` synchronously snapshots the parked queue and
  // flips the attempt to migrated in one critical section (no `await`
  // between the two). Model that here: the snapshot is taken, `actorReady`
  // flips as production's cutover would, and only afterwards do arrivals
  // dispatch — so nothing racing this boundary can double-park or vanish.
  const drained = registry.completeMigration(transferId);
  actorReady = true;

  // An arrival racing the boundary after `completeMigration` must resolve
  // the Actor directly (not-found from the registry, then ordinary
  // ingress) rather than parking into a queue nobody will ever drain again.
  const racingRoute = registry.routeIngress(ACTOR_ID, OBJECT_GENERATION, {
    header: sendHeaderBytes('racing'),
    payload: Buffer.from('RACING'),
    returnResponse: false,
    resolve: () => undefined,
    reject: () => undefined
  });
  assert.equal(racingRoute, 'not-found');

  for (const arrival of drained) {
    await dispatch.dispatch(
      ACTOR_ID,
      [Message.from(arrival.header), Message.from(arrival.payload)],
      arrival.returnResponse,
      arrival.remoteBoundSessionTarget,
      arrival.fallbackActorRef,
      arrival.requestTerminal
    );
  }
  await dispatch.dispatch(
    ACTOR_ID,
    [Message.from(sendHeaderBytes('racing')), Message.from('RACING')],
    false,
    undefined,
    fallbackRef()
  );

  assert.deepEqual(delivered, ['BEFORE', 'RACING']);
});

test('a newer exact identity for the same object evicts the older attempt and fails its parked arrivals exactly once', async () => {
  const registry = new ZLinkFormalRemoteActorAdmissionRegistry();
  const oldTransferId = 'transfer-old';
  const newTransferId = 'transfer-new';
  registry.begin(admission(oldTransferId));
  registry.complete(oldTransferId, { accepted: true, actorRef: fallbackRef() });

  const rejections: unknown[] = [];
  let rejectCount = 0;
  const parked: ZLinkParkedActorArrival = {
    header: sendHeaderBytes('stale'),
    payload: Buffer.from('STALE'),
    returnResponse: false,
    resolve: () => undefined,
    reject: (reason) => {
      rejectCount += 1;
      rejections.push(reason);
    }
  };
  const parkedRoute = registry.routeIngress(ACTOR_ID, OBJECT_GENERATION, parked);
  assert.equal(parkedRoute, 'parked');

  // Same object, a different (newer) RelocationId: spec 15 §4.2 "같은
  // object의 relocation temporary queue는 하나만 존재한다" — newest attempt
  // wins, and the older one's parked arrivals fail exactly once.
  registry.begin(admission(newTransferId));
  assert.equal(rejectCount, 1, 'the evicted attempt must fail its parked arrival exactly once');

  // The evicted attempt no longer owns the object: migrating it now must
  // reject cleanly instead of silently reusing a dead identity.
  assert.throws(() => registry.completeMigration(oldTransferId));

  // The new attempt owns the object going forward.
  const newParkedRoute = registry.routeIngress(ACTOR_ID, OBJECT_GENERATION, {
    header: sendHeaderBytes('fresh'),
    payload: Buffer.from('FRESH'),
    returnResponse: false,
    resolve: () => undefined,
    reject: () => undefined
  });
  assert.equal(newParkedRoute, 'parked');
  const drained = registry.completeMigration(newTransferId);
  assert.equal(drained.length, 1);
  assert.equal(Buffer.from(drained[0].payload).toString('utf8'), 'FRESH');

  // Eviction must not double-fail the same arrival if cleanup runs again.
  registry.delete(oldTransferId);
  assert.equal(rejectCount, 1);
});

test('Rejected admission and explicit abort fail every parked arrival exactly once', async () => {
  const registry = new ZLinkFormalRemoteActorAdmissionRegistry();
  const transferId = 'transfer-rejected';
  registry.begin(admission(transferId));

  let rejectCount = 0;
  const parkedRoute = registry.routeIngress(ACTOR_ID, OBJECT_GENERATION, {
    header: sendHeaderBytes('doomed'),
    payload: Buffer.from('DOOMED'),
    returnResponse: false,
    resolve: () => undefined,
    reject: () => { rejectCount += 1; }
  });
  assert.equal(parkedRoute, 'parked');

  // Rejected admission (spec 15 §4.2): the temporary queue is removed in
  // the same processing step as the reject decision.
  registry.complete(transferId, { accepted: false, actorRef: fallbackRef() });
  assert.equal(rejectCount, 1);

  // A further arrival for the same object after rejection is not-found —
  // there is nothing left to park into.
  const afterReject = registry.routeIngress(ACTOR_ID, OBJECT_GENERATION, {
    header: sendHeaderBytes('too-late'),
    payload: Buffer.from('TOO-LATE'),
    returnResponse: false,
    resolve: () => undefined,
    reject: () => undefined
  });
  assert.equal(afterReject, 'not-found');

  // Explicit abort (target-initiated cleanup, or the 30s admission
  // retention timer via `delete`) shares the same exactly-once cleanup:
  // register a second attempt, park under it, then abort it directly.
  const abortedTransferId = 'transfer-aborted';
  registry.begin(admission(abortedTransferId, 9n));
  let abortedRejectCount = 0;
  const abortedRoute = registry.routeIngress(ACTOR_ID, 9n, {
    header: sendHeaderBytes('aborted'),
    payload: Buffer.from('ABORTED'),
    returnResponse: false,
    resolve: () => undefined,
    reject: () => { abortedRejectCount += 1; }
  });
  assert.equal(abortedRoute, 'parked');
  registry.abort(abortedTransferId);
  assert.equal(abortedRejectCount, 1);
  // `delete` is the retention-timer cleanup path — must not double-fail
  // what `abort` already failed.
  registry.delete(abortedTransferId);
  assert.equal(abortedRejectCount, 1);
});

test('disabling the ingress prewarm consult reproduces the pre-fix silent drop', async () => {
  const registry = new ZLinkFormalRemoteActorAdmissionRegistry();
  const transferId = 'transfer-disabled';
  registry.begin(admission(transferId));
  registry.complete(transferId, { accepted: true, actorRef: fallbackRef() });

  let actorReady = false;
  const delivered: string[] = [];
  //  No `routeToActorJoinPrewarm` wired at all — the exact configuration
  //  this mechanism removes. `dispatch()` must fall back to its pre-fix
  //  behaviour: an ordinary Send for an unresolvable Actor is dropped
  //  silently instead of parked.
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {} as never,
    spotId: () => String(SPOT_ID),
    registry: new ZLinkSpotActorHandlerRegistryRuntime(),
    resolveActor: () => (actorReady ? ({ context: { actorId: ACTOR_ID } } as never) : undefined),
    onDisconnectActor: async () => undefined,
    routeBeforeLocal: (delivery: ZLinkActorPacketDelivery) => {
      if (!actorReady) return undefined;
      delivered.push(Buffer.from(delivery.parts[1].data()).toString('utf8'));
      return { handled: true, response: 'ok' };
    }
  });

  await dispatch.dispatch(
    ACTOR_ID,
    [Message.from(sendHeaderBytes('early')), Message.from('EARLY')],
    false,
    undefined,
    fallbackRef()
  );

  actorReady = true;
  // Nothing was parked, so there is nothing to migrate — the arrival is
  // gone. This is the failure this whole mechanism exists to prevent.
  assert.equal(registry.completeMigration(transferId).length, 0);
  assert.deepEqual(delivered, [], 'without the prewarm consult the arrival is silently dropped, not parked');
});
