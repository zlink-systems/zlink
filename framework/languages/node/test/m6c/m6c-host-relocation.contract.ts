import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Message, SubmitResult } from '@zlink-systems/zlink';
import { ZLinkSpotKind } from '../../packages/framework/src/contracts';
import type { ZLinkAuthoritySnapshot } from '../../packages/framework/src/contracts/Locations';
import {
  createServiceRelocationId,
  ZLinkHostServiceRelocationRuntime
} from '../../packages/framework/src/runtime/host/service-relocation-host-runtime';
import {
  decodeServiceRelocationControlRequest,
  decodeServiceRelocationControlResponse,
  encodeServiceRelocationControlRequest
} from '../../packages/framework/src/runtime/host/service-relocation-control';
import {
  decodeSessionRelocationSealed,
  encodeServiceWireFrozenActorApplicationRecord,
  encodeSessionRelocationRoute,
  encodeSessionRelocationSeal,
  type ServiceMaintenanceRelocationControl,
  type ServiceMaintenanceRelocationCutover,
  type ServiceMaintenanceRelocationData,
  type ServiceMaintenanceRelocationFailed,
  type ServiceMaintenanceRelocationPrepare,
  type ServiceMaintenanceRelocationReady,
  type ServiceMaintenanceRelocationState,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';
import { crc32c } from '../../packages/framework/src/runtime/foundation/service-relocation-runtime';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import { ZLinkActorTransferRuntime } from '../../packages/framework/src/runtime/host/actor-transfer-runtime';
import { ZLinkActorSessionBindingRegistry } from '../../packages/framework/src/runtime/streams/actor-session-binding-registry';
import { encodeActorAuthorityIdentity } from '../../packages/framework/src/runtime/actors/actor-authority-publication';
import { createInitialActorMessageFollowContext } from '../../packages/framework/src/runtime/actors/actor-message-follow-context';
import { ReceiveKind } from '../../packages/framework/src/runtime/foundation/service-runtime-contracts';
import { DefaultZLinkSpotManager } from '../../packages/framework/src/runtime/spots';
import { ZLinkFormalRemoteActorAdmissionRegistry } from '../../packages/framework/src/runtime/spots/formal-remote-actor-admission-registry';
import { encodeRelocationBaseBundleFramed } from '../../packages/framework/src/runtime/host/relocation-state-adapter';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';

const coordinator = {
  ownerId: 'source-owner',
  leaseGeneration: 3n,
  nodeRid: 'source',
  nodeGeneration: 2n,
  expectedAuthorityStoreVersion: 'store-v17'
} as const;

const target = {
  nodeRid: 'target',
  nodeGeneration: 6n,
  ownerId: 'target-owner',
  ownerLeaseGeneration: 14n
} as const;

const object = {
  kind: 'actor',
  actorId: 'actor-1',
  objectGeneration: 5n,
  expectedAuthorityOwnerGeneration: 11n
} as const;

test('relocation identity retries zero and local collisions with all 128 entropy bits', () => {
  const zero = Buffer.alloc(16);
  const collision = Buffer.from('00112233445566778899aabbccddeeff', 'hex');
  const accepted = Buffer.from('ffeeddccbbaa99887766554433221100', 'hex');
  const entropy = [zero, collision, accepted];
  const observed: string[] = [];
  const collisionId = '00112233-4455-6677-8899-aabbccddeeff';
  const acceptedId = 'ffeeddcc-bbaa-9988-7766-554433221100';

  const id = createServiceRelocationId(
    candidate => {
      observed.push(candidate);
      return candidate === collisionId;
    },
    size => {
      assert.equal(size, 16);
      return entropy.shift()!;
    }
  );

  assert.equal(id, acceptedId);
  assert.deepEqual(observed, [collisionId, acceptedId]);
  assert.equal(entropy.length, 0);
});

test('Session owner applies an exact relocation without an Actor authority or lease mirror', async () => {
  const registry = new ZLinkActorSessionBindingRegistry();
  const context = {
    routingId: 'session-1',
    bindLocal() {},
    unbindLocal() {}
  };
  const actor = {
    actorId: 'actor-session-exact',
    ref: {
      actorId: 'actor-session-exact',
      objectGeneration: 7n,
      bindingGeneration: 3n,
      nodeRid: 'source-node',
      ownerNodeGeneration: 2n
    }
  };
  registry.bind(context, actor, 'binding-token');
  await registry.sealRelocation({
    actorId: actor.actorId,
    actorGeneration: 7n,
    bindingGeneration: 3n,
    sessionIdentity: 'session-1',
    actorNodeRid: 'source-node',
    actorNodeGeneration: 2n,
    sealId: 'relocation-1'
  }, {
    objectGeneration: 7n,
    bindingGeneration: 3n
  });

  const snapshot = registry.relocationSnapshot(actor.actorId, 'relocation-1')!;
  assert.equal('actorOwnershipGeneration' in snapshot, false);
  assert.equal('ownerLeaseGeneration' in snapshot, false);
  await registry.applyRelocation(
    actor.actorId,
    'relocation-1',
    'route-fingerprint',
    'commit',
    async () => {
      assert.equal(registry.abortSeal(actor.actorId, 'relocation-1'), true);
    },
    {
      actorId: actor.actorId,
      objectGeneration: 7n,
      actorNodeRid: 'target-node',
      actorNodeGeneration: 4n,
      sessionIdentity: 'session-1',
      bindingGeneration: 3n
    }
  );
  registry.observeRelocationTerminal(actor.actorId, 'relocation-1', 'route-fingerprint');
});

test('pre-cutover rollback restores the source queue and state before one-way Session abort', async () => {
  const events: string[] = [];
  const actor = { context: { actorId: 'actor-rollback-order', meshName: 'mesh-a' } };
  let remoteTarget = {
    routerChannelId: 'mesh-a',
    targetNodeRid: 'session-owner',
    sessionNodeRid: 'session-owner',
    sessionRid: 'session-1',
    spotId: 'session-entry',
    bindingGeneration: 6n
  };
  const state = {
    actorType: 'Player',
    meshName: 'mesh-a',
    spotId: 'source-spot',
    nativeActorRef: {
      actorId: actor.context.actorId,
      generation: 5n,
      nodeRid: 'source-node'
    },
    locationGeneration: 11n,
    ownerLeaseGeneration: 3n,
    get remoteBoundSessionTarget() { return remoteTarget; },
    get boundSessionTransferTarget() { return undefined; },
    setRemoteBoundSessionTarget(value: typeof remoteTarget) { remoteTarget = value; },
    beginMove() { events.push('move-begun'); },
    endMove() { events.push('state-restored'); }
  };
  const authority: ZLinkAuthoritySnapshot = {
    kind: 'snapshot',
    storeVersion: { value: 'actor-v11' } as never,
    payload: Buffer.alloc(0),
    objectGeneration: 5n,
    authorityOwnerGeneration: 11n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 3n,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'mesh-a', rid: 'source-node' },
      descriptorLifecycleGeneration: 2n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date()
  };
  const runtime = new ZLinkActorTransferRuntime({
    spotManager: () => ({
      async dispatchRoutedActorPacket() {
        return { handled: true };
      }
    }),
    actorManager: () => undefined,
    primaryMeshNode: () => ({
      status: () => ({ routingId: 'source-node', lifecycleGeneration: 2n })
    }),
    async notifyEntrySpotActorLeft() {},
    async restoreEntrySpotActorJoined() {},
    locationLifecycle: () => undefined,
    actorHandoff: {
      begin() {},
      sealConnectionBoundIngress() {},
      snapshot() { return []; },
      takeRelocationRelay() { return []; },
      async releaseCanceled() { events.push('queue-restored'); }
    },
    actorTransferRegistry: {},
    authorityStore: () => ({ readAuthority: async () => authority }),
    relocationStore: () => undefined,
    liveDescriptors: async () => [{
      rid: 'session-owner',
      lifecycleGeneration: 4n,
      ownerId: 'session-owner-id',
      leaseGeneration: 8n
    }],
    sessionRelocationWire: () => ({
      async requestSessionRelocationSeal() {
        events.push('session-sealed');
        return undefined as never;
      },
      async sendSessionRelocationRoute(
        _meshName: string,
        _targetNodeRid: unknown,
        route: ServiceSessionRelocationRoute
      ) {
        assert.equal(route.route.action, 'abort');
        events.push('session-abort');
      }
    }),
    clearRemoteActorPacketTarget() {}
  } as never);

  const prepared = await runtime.prepareMaintenanceSession(
    actor as never,
    state as never,
    undefined,
    false,
    { high: 7n, low: 9n }
  );
  await prepared.rollback();
  await Promise.resolve();

  assert.deepEqual(events, [
    'move-begun',
    'session-sealed',
    'queue-restored',
    'state-restored',
    'session-abort'
  ]);
});

test('Session relocation is exact 42-to-43 and command 44 is one-way', async () => {
  const seal: ServiceSessionRelocationSeal = {
    relocation: { high: 7n, low: 9n },
    coordinator,
    senderRole: 'source',
    actor: {
      actor: { nodeRid: 'source', actorId: 'actor-1', generation: 5n },
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 3n
    },
    session: {
      sessionOwnerNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 4n,
      sessionOwnerId: 'session-owner-id',
      sessionOwnerLeaseGeneration: 8n,
      sessionRid: 'session-1',
      bindingGeneration: 6n
    }
  };
  const sealed: ServiceSessionRelocationSealed = {
    relocation: seal.relocation,
    coordinator: seal.coordinator,
    actor: seal.actor,
    session: seal.session
  };
  const route: ServiceSessionRelocationRoute = {
    relocation: seal.relocation,
    coordinator,
    senderRole: 'target',
    actor: { nodeRid: 'target', actorId: 'actor-1', generation: 5n },
    session: seal.session,
    route: {
      action: 'commit',
      previousAuthorityOwnerGeneration: 11n,
      targetAuthorityOwnerGeneration: 12n,
      targetNodeRid: 'target',
      targetNodeGeneration: 6n
    }
  };
  //  A relocation whose Actor owner IS the session owner: the 42/44
  //  controls target the local node and dispatch through the inbound
  //  handler (RouteMesh has no self connection), so their fences carry the
  //  local node identity.
  const selfSeal: ServiceSessionRelocationSeal = {
    ...seal,
    coordinator: { ...coordinator, nodeRid: 'session-owner', nodeGeneration: 4n },
    actor: {
      ...seal.actor,
      actor: { ...seal.actor.actor, nodeRid: 'session-owner' },
      targetNodeGeneration: 4n
    }
  };
  const selfSealed: ServiceSessionRelocationSealed = {
    relocation: selfSeal.relocation,
    coordinator: selfSeal.coordinator,
    actor: selfSeal.actor,
    session: selfSeal.session
  };
  const selfRoute: ServiceSessionRelocationRoute = {
    ...route,
    route: {
      ...route.route,
      targetNodeRid: 'session-owner',
      targetNodeGeneration: 4n
    }
  };
  const sent: Array<{ readonly target: string; readonly bytes: Buffer }> = [];
  const received: string[] = [];
  const runtime = new ZLinkHostServiceRelocationRuntime({
    currentOwner: () => ({ ownerId: 'session-owner-id', leaseGeneration: 8n }),
    meshNode: () => ({
      status: () => ({ routingId: 'session-owner', lifecycleGeneration: 4n }),
      peers: () => [
        { routingId: 'source', lifecycleGeneration: 2n, state: 3 },
        { routingId: 'target', lifecycleGeneration: 6n, state: 3 }
      ],
      sendToNode: (targetRid: string, bytes: Uint8Array) => {
        sent.push({ target: targetRid, bytes: Buffer.from(bytes) });
        return SubmitResult.Ok;
      }
    }),
    boundSessionRelocation: {
      receiveSeal: async (value: ServiceSessionRelocationSeal) => {
        received.push('seal');
        if (received.filter(entry => entry === 'seal').length === 1) {
          assert.deepEqual(value, seal);
          return sealed;
        }
        assert.deepEqual(value, selfSeal);
        return selfSealed;
      },
      receiveRoute: async (value: ServiceSessionRelocationRoute) => {
        received.push('route');
        //  The wire decode clears the sender actor nodeRid; a locally
        //  dispatched self-target control arrives unchanged.
        if (received.filter(entry => entry === 'route').length === 1) {
          assert.deepEqual(value, { ...route, actor: { ...route.actor, nodeRid: '' } });
        } else {
          assert.deepEqual(value, selfRoute);
        }
      }
    }
  } as never);
  const dispatch = async (sourceNodeRid: string, bytes: Uint8Array) => {
    const part = Message.from(bytes);
    try {
      return await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid,
        parts: [part]
      } as never);
    } finally {
      part.close();
    }
  };

  try {
    assert.equal(await dispatch('source', encodeSessionRelocationSeal(seal)), true);
    assert.deepEqual(received, ['seal']);
    assert.equal(sent.length, 1);
    assert.equal(sent[0]!.target, 'source');
    assert.deepEqual(decodeSessionRelocationSealed(sent[0]!.bytes), sealed);

    assert.equal(await dispatch('target', encodeSessionRelocationRoute(route)), true);
    assert.deepEqual(received, ['seal', 'route']);
    assert.equal(sent.length, 1, 'command 44 must not produce a routed ACK');

    //  This runtime is the session owner itself: a self-target 42/44 has
    //  no RouteMesh self connection and dispatches through the inbound
    //  handler locally without producing a wire submit.
    const pending = runtime.requestSessionRelocationSeal(
      'mesh-a', 'session-owner', selfSeal
    );
    assert.equal(
      pending,
      runtime.requestSessionRelocationSeal('mesh-a', 'session-owner', selfSeal),
      'identical command 42 requests must share one waiter'
    );
    assert.deepEqual(await pending, selfSealed);
    assert.equal(sent.length, 1, 'self-target command 42 dispatches locally');
    assert.deepEqual(received, ['seal', 'route', 'seal']);

    await runtime.sendSessionRelocationRoute('mesh-a', 'session-owner', selfRoute);
    assert.equal(sent.length, 1, 'self-target command 44 dispatches locally');
    assert.deepEqual(received, ['seal', 'route', 'seal', 'route']);
  } finally {
    await runtime.dispose();
  }
});

test('exact duplicate Prepare shares restore while Data and Cutover stay one-way', async () => {
  const prepare: ServiceMaintenanceRelocationPrepare = {
    kind: 'prepare',
    relocation: { high: 17n, low: 19n },
    targetAttemptGeneration: 1n,
    coordinator,
    target,
    initiatorRole: 'source',
    object,
    sourceNodeRid: 'source',
    sourceNodeGeneration: 2n,
    payloadTotalLength: 24n,
    payloadChunkCount: 1,
    payloadChecksumCrc32c: 123,
    baseChecksumCrc32c: 0,
    applicationVersion: 4n
  };
  const ready: ServiceMaintenanceRelocationReady = {
    kind: 'ready',
    relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration,
    coordinator,
    target,
    object,
    senderRole: 'target'
  };
  const sent: Buffer[] = [];
  const oneWay: string[] = [];
  let prepareCalls = 0;
  let release!: () => void;
  const held = new Promise<void>(resolve => { release = resolve; });
  const runtime = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => ({
      sendToNode: (_targetRid: string, bytes: Uint8Array) => {
        sent.push(Buffer.from(bytes));
        return SubmitResult.Ok;
      }
    })
  } as never);
  const internals = runtime as unknown as {
    handlePrepareControl: () => Promise<ServiceMaintenanceRelocationReady>;
    handleOneWayControl: (
      meshName: string,
      request: Extract<ServiceMaintenanceRelocationControl, { kind: 'data' | 'cutover' }>
    ) => Promise<void>;
  };
  internals.handlePrepareControl = async () => {
    prepareCalls += 1;
    await held;
    return ready;
  };
  internals.handleOneWayControl = async (_meshName, request) => {
    oneWay.push(request.kind);
  };
  const dispatch = async (request: ServiceMaintenanceRelocationControl) => {
    const part = Message.from(encodeServiceRelocationControlRequest(request));
    try {
      return await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid: 'source',
        parts: [part]
      } as never);
    } finally {
      part.close();
    }
  };

  try {
    const first = dispatch(prepare);
    const second = dispatch(prepare);
    assert.equal(prepareCalls, 1);
    await assert.rejects(
      dispatch({ ...prepare, applicationVersion: 5n }),
      /repeated Prepare with different bytes/
    );
    release();
    assert.deepEqual(await Promise.all([first, second]), [true, true]);
    assert.equal(prepareCalls, 1);
    // The Ready reply is delivered asynchronously once the shared restore
    // operation completes; each exact Prepare receipt answers once.
    await waitUntil(() => sent.length === 2);
    for (const bytes of sent) {
      assert.deepEqual(decodeServiceRelocationControlResponse(bytes), ready);
    }

    const frozenRecord = encodeServiceWireFrozenActorApplicationRecord({
      source: coordinator,
      target: {
        actorId: object.actorId,
        objectGeneration: object.objectGeneration,
        nodeRid: target.nodeRid,
        nodeGeneration: target.nodeGeneration,
        authorityOwnerGeneration: 12n,
        ownerLeaseGeneration: target.ownerLeaseGeneration
      },
      operationId: { high: 23n, low: 29n },
      payload: {
        packetName: '__zlink.actor.handoff.accepted',
        contentType: 'application/json',
        bytes: Buffer.from('{}')
      }
    });
    assert.equal(await dispatch({
      kind: 'data',
      relocation: prepare.relocation,
      targetAttemptGeneration: 1n,
      coordinator,
      senderRole: 'source',
      object,
      frozenRecord
    }), true);
    assert.equal(await dispatch({
      kind: 'cutover',
      relocation: prepare.relocation,
      targetAttemptGeneration: 1n,
      coordinator,
      senderRole: 'source',
      object,
      boundaryRecordCount: 1n,
      boundaryChecksumCrc32c: 0
    }), true);
    assert.deepEqual(oneWay, ['data', 'cutover']);
    assert.equal(sent.length, 2, 'commands 31 and 34 must not send responses');
  } finally {
    await runtime.dispose();
  }
});

test('an explicit Failed(53) rejects the pending Prepare ACK promptly with its classified failure, ' +
  'and never resolves a different relocation identity\'s ACK', async () => {
  // Spec 28 §9: an explicit Failed reply restores source memory promptly —
  // the source must not learn the outcome only from its own resend-loop
  // deadline. Node's wire failureCode vocabulary distinguishes DataLost(35)
  // from the generic InternalFailure code requestFailed(17); the source-side
  // classification must track whichever one the target actually sent.
  const buildPrepare = (relocation: { high: bigint; low: bigint }): ServiceMaintenanceRelocationPrepare => ({
    kind: 'prepare',
    relocation,
    targetAttemptGeneration: 1n,
    coordinator,
    target,
    initiatorRole: 'source',
    object,
    sourceNodeRid: 'source',
    sourceNodeGeneration: 2n,
    payloadTotalLength: 24n,
    payloadChunkCount: 1,
    payloadChecksumCrc32c: 123,
    baseChecksumCrc32c: 0,
    applicationVersion: 4n
  });
  const deliverFailed = async (
    runtime: ZLinkHostServiceRelocationRuntime,
    failed: ServiceMaintenanceRelocationFailed
  ) => {
    const part = Message.from(encodeServiceRelocationControlRequest(failed));
    try {
      await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid: 'target',
        parts: [part]
      } as never);
    } finally {
      part.close();
    }
  };
  const pendingCount = (runtime: ZLinkHostServiceRelocationRuntime) =>
    (runtime as unknown as { pendingControls: Map<string, unknown> }).pendingControls.size;

  for (const [failureCode, expectedKind] of [
    [35, ZLinkFrameworkErrorKind.DataLost],
    [17, ZLinkFrameworkErrorKind.InternalFailure]
  ] as const) {
    const runtime = new ZLinkHostServiceRelocationRuntime({
      meshNode: () => ({ sendToNode: () => SubmitResult.Ok })
    } as never);
    const internals = runtime as unknown as {
      sendControl: (
        meshName: string,
        targetNodeRid: string,
        request: ServiceMaintenanceRelocationPrepare,
        signal: AbortSignal | undefined,
        deadlineAtMs: number
      ) => Promise<unknown>;
    };
    const prepare = buildPrepare({ high: 71n, low: BigInt(failureCode) });
    try {
      // A one-minute deadline: a prompt rejection proves it came from the
      // Failed reply, not the resend-loop timeout.
      const ack = internals.sendControl('mesh-a', 'target', prepare, undefined, Date.now() + 60_000);
      ack.catch(() => undefined);
      assert.equal(pendingCount(runtime), 1, 'the Prepare ACK must be pending');

      // A Failed for an unrelated relocation identity must never resolve it.
      await deliverFailed(runtime, {
        kind: 'failed',
        relocation: { high: 999n, low: 999n },
        targetAttemptGeneration: 1n,
        coordinator,
        target,
        object,
        senderRole: 'target',
        failureCode
      });
      assert.equal(
        pendingCount(runtime), 1,
        'a Failed for a different relocation identity must not resolve the pending ACK'
      );

      const started = Date.now();
      await deliverFailed(runtime, {
        kind: 'failed',
        relocation: prepare.relocation,
        targetAttemptGeneration: prepare.targetAttemptGeneration,
        coordinator,
        target,
        object,
        senderRole: 'target',
        failureCode
      });
      await assert.rejects(ack, (error: unknown) => {
        assert.ok(error instanceof ZLinkFrameworkException, 'must reject with a typed framework exception');
        assert.equal(error.kind, expectedKind, `failureCode ${failureCode} must classify as ${expectedKind}`);
        return true;
      });
      assert.ok(
        Date.now() - started < 1_000,
        'the explicit Failed must resolve the ACK promptly, not via the 60s deadline'
      );
      assert.equal(pendingCount(runtime), 0, 'the resolved ACK must be removed from the pending map');
    } finally {
      await runtime.dispose();
    }
  }
});

test('cutover boundary reconciliation replaces a stale pre-reconnect span with the retransmitted whole batch', async () => {
  // Spec 28 §4.4: a retransmission after reconnect always resends the entire
  // boundary batch. If the target already buffered a partially received span
  // from before the disconnect, the retransmitted batch (declared by the
  // cutover's boundaryRecordCount/boundaryChecksumCrc32c) must replace it as
  // a whole rather than being appended after it.
  const runtime = new ZLinkHostServiceRelocationRuntime({} as never);
  const internals = runtime as unknown as {
    reconcileBoundaryRelay: (
      stage: { boundaryRelay: ServiceMaintenanceRelocationData[] },
      request: ServiceMaintenanceRelocationCutover,
      stagingId: string
    ) => void;
  };
  const record = (label: string): ServiceMaintenanceRelocationData => ({
    kind: 'data',
    relocation: { high: 0n, low: 1n },
    targetAttemptGeneration: 1n,
    coordinator,
    senderRole: 'source',
    object,
    frozenRecord: { canonicalBytes: Buffer.from(label) }
  } as unknown as ServiceMaintenanceRelocationData);

  try {
    const stale = [record('stale-0'), record('stale-1')];
    const retransmitted = [record('whole-0'), record('whole-1'), record('whole-2')];
    const stage = { boundaryRelay: [...stale, ...retransmitted] };
    const boundaryChecksumCrc32c = crc32c(
      Buffer.concat(retransmitted.map(value => value.frozenRecord.canonicalBytes))
    );
    const cutover: ServiceMaintenanceRelocationCutover = {
      kind: 'cutover',
      relocation: { high: 0n, low: 1n },
      targetAttemptGeneration: 1n,
      coordinator,
      senderRole: 'source',
      object,
      boundaryRecordCount: BigInt(retransmitted.length),
      boundaryChecksumCrc32c
    };

    internals.reconcileBoundaryRelay(stage, cutover, 'stage-retransmit');

    assert.deepEqual(stage.boundaryRelay, retransmitted);
  } finally {
    await runtime.dispose();
  }
});

test('cutover boundary reconciliation throws on an unordered-connection defect that no retransmission can explain', async () => {
  const runtime = new ZLinkHostServiceRelocationRuntime({} as never);
  const internals = runtime as unknown as {
    reconcileBoundaryRelay: (
      stage: { boundaryRelay: ServiceMaintenanceRelocationData[] },
      request: ServiceMaintenanceRelocationCutover,
      stagingId: string
    ) => void;
  };
  const record = (label: string): ServiceMaintenanceRelocationData => ({
    kind: 'data',
    relocation: { high: 0n, low: 1n },
    targetAttemptGeneration: 1n,
    coordinator,
    senderRole: 'source',
    object,
    frozenRecord: { canonicalBytes: Buffer.from(label) }
  } as unknown as ServiceMaintenanceRelocationData);

  try {
    const stage = { boundaryRelay: [record('only-0')] };
    const cutover: ServiceMaintenanceRelocationCutover = {
      kind: 'cutover',
      relocation: { high: 0n, low: 1n },
      targetAttemptGeneration: 1n,
      coordinator,
      senderRole: 'source',
      object,
      boundaryRecordCount: 2n,
      boundaryChecksumCrc32c: 0
    };

    assert.throws(
      () => internals.reconcileBoundaryRelay(stage, cutover, 'stage-mismatch'),
      /boundary confirmation mismatch/
    );
  } finally {
    await runtime.dispose();
  }
});

test('target-only CAS reconciles an unknown response to the exact committed owner', async () => {
  const actorKey = encodeAuthorityKey('actor', object.actorId);
  const envelope = {
    aggregateId: '00000000-0000-0000-0000-000000000011',
    aggregateGeneration: 1n,
    participants: [{
      key: actorKey.value,
      objectKind: 'actor',
      stableType: 'Player',
      objectGeneration: object.objectGeneration,
      authorityOwnerGeneration: object.expectedAuthorityOwnerGeneration,
      applicationState: Buffer.alloc(0),
      boundSessionState: Buffer.alloc(0),
      queuedMessages: [],
      timers: []
    }],
    memberships: [{
      actorKey: actorKey.value,
      spotKey: encodeAuthorityKey('user_spot', 'entry-target').value,
      spotObjectGeneration: 6n,
      membershipEpoch: 1n
    }]
  } as const;
  const expected: ZLinkAuthoritySnapshot = {
    kind: 'snapshot',
    storeVersion: { value: 'source-v1' } as never,
    payload: Buffer.alloc(0),
    objectGeneration: object.objectGeneration,
    authorityOwnerGeneration: object.expectedAuthorityOwnerGeneration,
    ownerId: coordinator.ownerId,
    ownerLeaseGeneration: coordinator.leaseGeneration,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'mesh-a', rid: 'source' },
      descriptorLifecycleGeneration: coordinator.nodeGeneration,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date()
  };
  const prepare: ServiceMaintenanceRelocationPrepare = {
    kind: 'prepare',
    relocation: { high: 0n, low: 17n },
    targetAttemptGeneration: 1n,
    coordinator,
    target,
    initiatorRole: 'source',
    object,
    sourceNodeRid: coordinator.nodeRid,
    sourceNodeGeneration: coordinator.nodeGeneration,
    payloadTotalLength: 24n,
    payloadChunkCount: 1,
    payloadChecksumCrc32c: 1,
    baseChecksumCrc32c: 0,
    applicationVersion: 4n
  };
  let current = expected;
  let writes = 0;
  const authorityPayload = Buffer.from('target-authority');
  const runtime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => ({
      commitAggregate: async () => {
        writes += 1;
        current = {
          ...expected,
          storeVersion: { value: 'target-v2' } as never,
          payload: authorityPayload,
          authorityOwnerGeneration: expected.authorityOwnerGeneration + 1n,
          ownerId: target.ownerId,
          ownerLeaseGeneration: target.ownerLeaseGeneration,
          allocation: {
            ...expected.allocation,
            descriptor: { meshName: 'mesh-a', rid: target.nodeRid },
            descriptorLifecycleGeneration: target.nodeGeneration
          }
        };
        throw new Error('commit response lost');
      },
      readAuthority: async () => current
    })
  } as never);
  const committed = await (runtime as unknown as {
    commitTargetReservation(
      stage: unknown,
      reservation: unknown
    ): Promise<ZLinkAuthoritySnapshot>;
  }).commitTargetReservation({
    offer: {
      prepare,
      prepareFingerprint: 'prepare',
      authenticatedSourceNodeRid: coordinator.nodeRid,
      envelope,
      restoreDeadlineAtMs: Date.now() + 10_000
    },
    staging: { primaryAuthorityKey: actorKey, envelope }
  }, {
    prepared: {
      fence: {
        aggregateId: { value: envelope.aggregateId },
        aggregateGeneration: envelope.aggregateGeneration
      },
      plan: {
        envelope,
        participants: [{
          key: actorKey,
          expected,
          ownerTransition: 'newOwner',
          authorityPayload,
          membershipMutation: Buffer.from('membership')
        }],
        targetDescriptor: { meshName: 'mesh-a', rid: target.nodeRid },
        targetDescriptorLifecycleGeneration: target.nodeGeneration,
        capacity: expected.allocation.capacity,
        targetOwner: {
          ownerId: target.ownerId,
          leaseGeneration: target.ownerLeaseGeneration
        }
      }
    }
  });

  assert.equal(writes, 1);
  assert.equal(committed.ownerId, target.ownerId);
  assert.equal(committed.authorityOwnerGeneration, 12n);
});

test('ActorJoin profile reuses the Host terminal owner and a failed one-way source leave submit does not gate Accepted', async () => {
  const events: string[] = [];
  const actor = { context: { actorId: 'actor-join', meshName: 'mesh-a' } };
  const nativeRef = { actorId: 'actor-join', generation: 5n, nodeRid: 'target' };
  const authority = {
    kind: 'snapshot',
    storeVersion: { value: 'target-v2' } as never,
    payload: Buffer.alloc(0),
    objectGeneration: 5n,
    authorityOwnerGeneration: 12n,
    ownerId: 'target-owner',
    ownerLeaseGeneration: 14n,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'mesh-a', rid: 'target' },
      descriptorLifecycleGeneration: 6n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date()
  } as ZLinkAuthoritySnapshot;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    actorManager: () => ({ getState: () => ({ nativeActorRef: nativeRef }) }),
    spotManager: () => ({
      async finalizeActorJoinRelocation(
        _meshName: string,
        _relocationId: string,
        _actor: unknown,
        _actorRef: unknown,
        submitSourceLeave: (sourceNodeRid: string) => Promise<void>
      ) {
        events.push('onJoined');
        await submitSourceLeave('source');
        events.push('accepted');
        return true;
      }
    }),
    meshNode: () => ({
      async sendToNode() {
        events.push('sourceLeave:submit');
        return SubmitResult.NotConnected;
      }
    }),
    actorTransfer: {
      async publishRoutedActorOwnership() {
        events.push('command44');
      }
    }
  } as never);
  const internals = runtime as unknown as {
    commitTargetReservation: () => Promise<ZLinkAuthoritySnapshot>;
    relayTerminalReplies: () => Promise<void>;
    targetReplyRelayCoordinator: () => unknown;
    clearTargetRelocationPublication: () => Promise<void>;
    finalizeTargetStage: (
      meshName: string,
      stagingId: string,
      stage: unknown
    ) => Promise<void>;
  };
  internals.commitTargetReservation = async () => {
    events.push('cas');
    return authority;
  };
  internals.relayTerminalReplies = async () => {};
  internals.targetReplyRelayCoordinator = () => ({});
  internals.clearTargetRelocationPublication = async () => {};
  const envelope = {
    aggregateId: '00000000-0000-0000-0000-000000000021',
    aggregateGeneration: 1n,
    participants: [],
    memberships: []
  };
  const hidden = {
    authorityKey: encodeAuthorityKey('actor', actor.context.actorId).value,
    actor
  };
  const stage = {
    offer: { prepareFingerprint: 'actor-join' },
    owner: {
      async publish() { events.push('route:closed'); },
      async normalize() { events.push('queue:merged'); },
      async openAdmission() { events.push('dispatch:open'); }
    },
    staging: {
      envelope,
      hidden: new Map([[hidden.authorityKey, hidden]])
    },
    phase: 'ready',
    lane: Promise.resolve(),
    cutoverReceived: true,
    boundaryRelay: []
  };
  const originalWarn = console.warn;
  console.warn = () => events.push('sourceLeave:warning');
  try {
    await internals.finalizeTargetStage('mesh-a', 'actor-join-stage', stage);
  } finally {
    console.warn = originalWarn;
    await runtime.dispose();
  }

  assert.deepEqual(events, [
    'cas',
    'queue:merged',
    'route:closed',
    'onJoined',
    'sourceLeave:submit',
    'sourceLeave:warning',
    'accepted',
    'dispatch:open',
    'command44'
  ]);
});

test('public ActorJoin profile crosses the Host Prepare READY DATA CUTOVER owner before target completion', async () => {
  const harness = createActorJoinHostHarness({ holdAccepted: true, holdSourceLeave: true });
  try {
    const relocation = harness.relocate();
    const relocated = await relocation;
    assert.equal(String(relocated.actorRef.nodeRid), 'target');
    assert.equal(
      harness.events.includes('accepted:completed'),
      false,
      'the source relocation goal must not wait for target Accepted completion'
    );
    assert.equal(
      harness.events.includes('source:onLeave:completed'),
      false,
      'the source relocation goal must not wait for the one-way OnLeave callback'
    );
    assert.deepEqual(harness.controlKinds, ['prepare', 'state', 'data', 'cutover']);
    assert.equal(harness.location.commits, 1, 'only the target owner may commit Location CAS');
    assert.equal(harness.targetActorManager.published, 0, 'dispatch stays closed before Accepted');

    harness.releaseAccepted();
    await harness.targetIdle();
    assert.deepEqual(harness.events.filter(value =>
      value === 'cas'
      || value === 'route:closed'
      || value.startsWith('queue:merged:')
      || value === 'onJoined'
      || value === 'sourceLeave:submit'
      || value === 'accepted:completed'
      || value === 'dispatch:open'
      || value.startsWith('replay:')
      || value === 'command44'
    ), [
      'cas',
      'queue:merged:B1,B2,D1',
      'route:closed',
      'onJoined',
      'sourceLeave:submit',
      'accepted:completed',
      'dispatch:open',
      'replay:B1',
      'replay:B2',
      'replay:D1',
      'command44'
    ]);
    assert.equal(
      harness.events.includes('source:onLeave:completed'),
      false,
      'target Accepted, dispatch open, and command 44 must not wait for source callback completion'
    );

    const mutationCount = harness.location.commits;
    await harness.deliverCutoverAgain();
    assert.equal(harness.location.commits, mutationCount, 'late CUTOVER must have zero mutation');

    harness.releaseSourceLeave();
    await harness.sourceLeaveIdle();
    assert.equal(harness.events.at(-1), 'source:removed');
    assert.deepEqual(harness.sourceCleanupRefs, [{
      actorId: 'actor-host-profile',
      generation: 5n,
      nodeRid: 'source'
    }], 'source cleanup must use the exact pre-relocation native authority ref');
    assert.deepEqual(
      harness.sourceLeaveSpotIds,
      ['source-room'],
      'source leave must use the exact pre-relocation Spot identity'
    );
    const removals = harness.events.filter(value => value === 'source:removed').length;
    assert.equal(await harness.deliverSourceLeaveAgain(), true);
    await Promise.resolve();
    assert.equal(
      harness.events.filter(value => value === 'source:removed').length,
      removals,
      'duplicate source-leave terminal must be handled with zero mutation'
    );
  } finally {
    await harness.dispose();
  }
});

test('ActorJoin threads the admission-advertised chunk cap into the state chunk plan', async () => {
  // Spec 15 §4.2: the source uses min(configured, advertised, conservative
  // floor) for that join's relocation state chunks. A small advertised cap
  // (well below both the configured limit and the payload size) must split
  // the payload into more than the usual single chunk, proving the value
  // reached runCoordinator's chunk plan rather than being ignored.
  const harness = createActorJoinHostHarness();
  try {
    const relocated = await harness.relocate(16);
    await harness.targetIdle();
    assert.equal(String(relocated.actorRef.nodeRid), 'target');
    const stateChunkCount = harness.controlKinds.filter(kind => kind === 'state').length;
    assert.ok(
      stateChunkCount > 1,
      `a 16-byte advertised cap must split the payload into multiple chunks, got ${stateChunkCount}`
    );
    assert.equal(harness.location.commits, 1);
  } finally {
    await harness.dispose();
  }
});

test('ActorJoin Host owner arms the exact 1000ms target fallback after READY', async () => {
  const originalSetTimeout = globalThis.setTimeout;
  const observedFallbacks: number[] = [];
  globalThis.setTimeout = ((callback: (...args: any[]) => void, delay?: number, ...args: any[]) => {
    if (delay === 1_000) {
      observedFallbacks.push(delay);
      return originalSetTimeout(callback, 0, ...args);
    }
    return originalSetTimeout(callback, delay, ...args);
  }) as typeof setTimeout;
  const harness = createActorJoinHostHarness({ dropCutover: true });
  const originalWarn = console.warn;
  console.warn = (...args: unknown[]) => {
    if (args[0] === '[zlink.runtime.relocation.cutover_timeout]') {
      harness.events.push('fallback:1000');
    }
  };
  try {
    await harness.relocate();
    await harness.targetIdle();
    assert.deepEqual(harness.controlKinds, ['prepare', 'state', 'data', 'cutover']);
    // The source retransmission window uses the same configured 1,000 ms
    // value, so the patched timer observes at least the target fallback arm.
    assert.equal(observedFallbacks.includes(1_000), true);
    assert.equal(harness.events.includes('fallback:1000'), true);
    assert.equal(harness.location.commits, 1);
  } finally {
    console.warn = originalWarn;
    globalThis.setTimeout = originalSetTimeout;
    await harness.dispose();
  }
});

test('ActorJoin READY submit failure re-submits Ready against the retained staging on every Prepare resend', async () => {
  //  Spec 28: a READY reply that fails to submit is not a restore failure —
  //  the restored staging is retained, and an exact-identity Prepare resend
  //  re-submits READY against it instead of rolling back or terminating.
  //  Permanent non-delivery is bounded by the existing Restore validity
  //  window, not by this delivery path.
  const harness = createActorJoinHostHarness({ readyResult: SubmitResult.NotConnected });
  const relocatePromise = harness.relocate();
  relocatePromise.catch(() => undefined);
  try {
    await new Promise<void>(resolve => setTimeout(resolve, 900));
    assert.ok(
      harness.events.filter(value => value === 'ready:submit').length >= 2,
      'a Prepare resend must re-submit Ready against the retained staging'
    );
    assert.equal(harness.location.commits, 0, 'an undelivered Ready must never arm CAS fallback');
    assert.equal(harness.targetStageCount(), 1, 'the restored staging must be retained, not erased');
    assert.equal(harness.location.aborts, 0, 'a delivery retry must not roll back the restored staging');
    assert.equal(
      harness.targetActorManager.aborted, 0, 'hidden target restore must not roll back on a delivery retry'
    );
  } finally {
    await harness.dispose();
  }
});

test('exact ActorJoin Prepare can restore again and arm fallback only after READY resend succeeds', async () => {
  const harness = createActorJoinHostHarness({
    readyResults: [SubmitResult.NotConnected, SubmitResult.Ok]
  });
  try {
    await harness.relocate();
    harness.clearDeliveryErrors();
    await harness.targetIdle();
    assert.ok(harness.prepareFingerprints.length >= 2);
    assert.equal(
      new Set(harness.prepareFingerprints).size,
      1,
      'every Prepare retry must preserve the exact frozen bytes'
    );
    assert.equal(
      harness.targetActorManager.aborted, 0, 'a Ready-delivery retry reuses staging, never rolling it back'
    );
    assert.equal(harness.targetActorManager.published, 1);
    assert.equal(harness.location.commits, 1);
  } finally {
    await harness.dispose();
  }
});

test('an undelivered READY expires and cleans up the retained target stage exactly once', async () => {
  // Spec 28 §9 / finding F3: a Ready that never delivers is bounded by the
  // Restore validity window (300,000ms), not held forever.
  const originalSetTimeout = globalThis.setTimeout;
  let expiryArmed = 0;
  globalThis.setTimeout = ((callback: (...args: any[]) => void, delay?: number, ...args: any[]) => {
    if (delay === 300_000) {
      expiryArmed += 1;
      return originalSetTimeout(callback, 0, ...args);
    }
    return originalSetTimeout(callback, delay, ...args);
  }) as typeof setTimeout;
  const harness = createActorJoinHostHarness({ readyResult: SubmitResult.NotConnected });
  const relocatePromise = harness.relocate();
  relocatePromise.catch(() => undefined);
  try {
    await new Promise<void>(resolve => setTimeout(resolve, 200));
    assert.ok(expiryArmed >= 1, 'the Ready delivery must arm the Restore-validity expiry timer');
    assert.equal(
      harness.targetStageCount(),
      0,
      'the expired Ready must clean up the retained target stage exactly once'
    );
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    await harness.dispose();
  }
});

test('an orphaned pre-Prepare base buffer expires without a Prepare ever arriving', async () => {
  // Spec 15 §5, finding F3 pattern: base-stage chunks travel ahead of the
  // relocationPrepare manifest that finalizes them. If the source crashes
  // before sending Prepare, nothing else touches the staged buffer — it must
  // still be bounded by the same Restore validity window (300,000ms) as
  // every other pre-restore target state, not retained forever.
  const originalSetTimeout = globalThis.setTimeout;
  let expiryArmed = 0;
  globalThis.setTimeout = ((callback: (...args: any[]) => void, delay?: number, ...args: any[]) => {
    if (delay === 300_000) {
      expiryArmed += 1;
      return originalSetTimeout(callback, 0, ...args);
    }
    return originalSetTimeout(callback, delay, ...args);
  }) as typeof setTimeout;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => ({ sendToNode: () => SubmitResult.Ok })
  } as never);
  const internals = runtime as unknown as { targetBaseBuffers: Map<string, unknown> };
  const state: ServiceMaintenanceRelocationState = {
    kind: 'state',
    relocation: { high: 41n, low: 43n },
    targetAttemptGeneration: 1n,
    coordinator,
    senderRole: 'source',
    object,
    payloadStage: 'base',
    chunkOrdinal: 0,
    chunkData: Buffer.from('base-snapshot-bytes')
  };
  const part = Message.from(encodeServiceRelocationControlRequest(state));
  try {
    await runtime.tryHandleControl('mesh-a', {
      sourceNodeRid: 'source',
      parts: [part]
    } as never);
    assert.equal(internals.targetBaseBuffers.size, 1, 'the base chunk must stage a pending buffer');
    await new Promise<void>(resolve => setTimeout(resolve, 200));
    assert.ok(expiryArmed >= 1, 'staging the base chunk must arm the Restore-validity expiry timer');
    assert.equal(
      internals.targetBaseBuffers.size,
      0,
      'an orphaned base buffer must be reclaimed without a Prepare ever arriving'
    );
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    part.close();
    await runtime.dispose();
  }
});

test('a Prepare that resolves a base buffer consumes it exactly once and a later timer never clobbers ' +
  'a newer buffer under the same key', async () => {
  const armedBaseTimers: Array<() => void> = [];
  const originalSetTimeout = globalThis.setTimeout;
  globalThis.setTimeout = ((callback: (...args: any[]) => void, delay?: number, ...args: any[]) => {
    if (delay === 300_000) {
      armedBaseTimers.push(() => callback(...args));
      return { unref: () => undefined } as unknown as ReturnType<typeof setTimeout>;
    }
    return originalSetTimeout(callback, delay, ...args);
  }) as typeof setTimeout;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => ({ sendToNode: () => SubmitResult.Ok })
  } as never);
  const internals = runtime as unknown as {
    targetBaseBuffers: Map<string, unknown>;
    resolveTargetBasePayloads: (
      stagingId: string,
      request: ServiceMaintenanceRelocationPrepare
    ) => ReadonlyMap<string, Buffer> | undefined;
  };
  const stagingId = '41:43:1';
  const framed = encodeRelocationBaseBundleFramed(new Map([['authority-1', Buffer.from('base-authority-1')]]));
  const prepare: ServiceMaintenanceRelocationPrepare = {
    kind: 'prepare',
    relocation: { high: 41n, low: 43n },
    targetAttemptGeneration: 1n,
    coordinator,
    target,
    initiatorRole: 'source',
    object,
    sourceNodeRid: 'source',
    sourceNodeGeneration: 2n,
    payloadTotalLength: 24n,
    payloadChunkCount: 1,
    payloadChecksumCrc32c: 123,
    baseChecksumCrc32c: framed.checksumCrc32c,
    applicationVersion: 4n
  };
  const sendBaseChunk = async (chunkData: Buffer) => {
    const state: ServiceMaintenanceRelocationState = {
      kind: 'state',
      relocation: prepare.relocation,
      targetAttemptGeneration: prepare.targetAttemptGeneration,
      coordinator,
      senderRole: 'source',
      object,
      payloadStage: 'base',
      chunkOrdinal: 0,
      chunkData
    };
    const part = Message.from(encodeServiceRelocationControlRequest(state));
    try {
      await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid: 'source',
        parts: [part]
      } as never);
    } finally {
      part.close();
    }
  };
  try {
    await sendBaseChunk(framed.blob);
    assert.equal(internals.targetBaseBuffers.size, 1, 'the base chunk must stage a pending buffer');
    assert.equal(armedBaseTimers.length, 1, 'staging the base chunk must arm exactly one expiry timer');

    const resolved = internals.resolveTargetBasePayloads(stagingId, prepare);
    assert.deepEqual(
      [...(resolved ?? new Map())].map(([key, value]) => [key, value.toString()]),
      [['authority-1', 'base-authority-1']],
      'a Prepare arriving before the TTL must consume the staged base buffer exactly once'
    );
    assert.equal(
      internals.targetBaseBuffers.size, 0, 'a resolved base buffer must be removed immediately'
    );

    // A later attempt reusing the same relocation/attempt key stages a
    // genuinely newer buffer under the same operation key.
    await sendBaseChunk(Buffer.from('unrelated-newer-buffer'));
    assert.equal(internals.targetBaseBuffers.size, 1, 'the newer attempt must stage its own buffer');
    const newerBuffer = internals.targetBaseBuffers.get(stagingId);
    assert.equal(armedBaseTimers.length, 2, 'the newer attempt must arm its own expiry timer');

    // The first (now-stale) buffer's timer fires late: it must be a no-op
    // and must never clobber the newer buffer staged under the same key.
    armedBaseTimers[0]!();
    assert.equal(
      internals.targetBaseBuffers.get(stagingId), newerBuffer,
      'a late timer for a consumed/superseded buffer must never clobber a newer same-fence buffer'
    );

    // The newer buffer's own timer still reclaims it normally.
    armedBaseTimers[1]!();
    assert.equal(
      internals.targetBaseBuffers.size, 0, 'the newer buffer must still be reclaimed by its own timer'
    );
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    await runtime.dispose();
  }
});

test('a target-side restore failure delivers Failed(53) end to end and restores source memory ' +
  'before any deadline', async () => {
  // Spec 28 §9: an explicit Failed must be consumed promptly by the source's
  // pending Prepare ACK (not discovered only via timeout), and because Ready
  // never arrived, runCoordinator's finally must restore (abort) the source
  // authority from the retained in-memory payload.
  const harness = createActorJoinHostHarness({});
  harness.targetActorManager.prepareRelocationActor = async () => {
    throw new Error('Target factory failed for this test.');
  };
  const originalWarn = console.warn;
  const originalError = console.error;
  console.warn = () => {};
  console.error = () => {};
  try {
    const started = Date.now();
    await assert.rejects(harness.relocate(), (error: unknown) => {
      assert.ok(error instanceof ZLinkFrameworkException, 'must reject with a typed framework exception');
      assert.equal(
        error.kind, ZLinkFrameworkErrorKind.InternalFailure,
        'a factory failure classifies as requestFailed(17) -> InternalFailure'
      );
      return true;
    });
    assert.ok(
      Date.now() - started < 5_000,
      'the explicit Failed must resolve the relocation well before the 30s control deadline'
    );
    assert.equal(harness.location.aborts, 1, 'the source authority must be restored from memory');
  } finally {
    console.warn = originalWarn;
    console.error = originalError;
    await harness.dispose();
  }
});

test('ActorJoin source profile reaches the existing Message Follow terminal after leave submit failure', async () => {
  const harness = createActorJoinHostHarness({
    sourceLeaveResult: SubmitResult.NotConnected
  });
  const originalWarn = console.warn;
  console.warn = () => {};
  try {
    await harness.relocate();
    await harness.targetIdle();
    assert.equal(harness.sourceProfileCount(), 1);

    harness.completeSourceCleanup();
    await harness.sourceLeaveIdle();
    assert.equal(harness.sourceProfileCount(), 0);
    assert.deepEqual(harness.events.filter(value =>
      value.startsWith('source:onLeave') || value === 'source:removed'
    ), [
      'source:onLeave:started',
      'source:onLeave:completed',
      'source:removed'
    ]);
  } finally {
    console.warn = originalWarn;
    await harness.dispose();
  }
});

interface ActorJoinHarnessOptions {
  readonly holdAccepted?: boolean;
  readonly holdSourceLeave?: boolean;
  readonly readyResult?: number;
  readonly readyResults?: number[];
  readonly sourceLeaveResult?: number;
  readonly dropCutover?: boolean;
}

function createActorJoinHostHarness(options: ActorJoinHarnessOptions = {}) {
  const events: string[] = [];
  const controlKinds: string[] = [];
  const prepareFingerprints: string[] = [];
  const relocationId = '00000000-0000-0000-0000-000000000091';
  const actorId = 'actor-host-profile';
  const completionOperationId = { high: 61n, low: 67n };
  const sourceActor = { context: { actorId, meshName: 'mesh-a' } };
  const sourceActorRef = {
    actorId,
    objectGeneration: 5n,
    meshName: 'mesh-a',
    nodeRid: 'source'
  };
  const targetDescriptor = {
    rid: 'target',
    lifecycleGeneration: 6n,
    ownerId: 'target-owner',
    leaseGeneration: 14n,
    applicationVersion: 4n,
    entrySpotId: 'entry-target'
  };
  const sourceAuthority: ZLinkAuthoritySnapshot = {
    kind: 'snapshot',
    storeVersion: { value: 'source-v1' } as never,
    payload: encodeActorAuthorityIdentity({
      actorType: 'Player',
      actor: sourceActorRef as never,
      meshName: 'mesh-a',
      ownerNodeGeneration: 2n,
      owner: { ownerId: 'source-owner', leaseGeneration: 3n },
      spotId: 'source-room',
      spotGeneration: 4n
    }),
    objectGeneration: 5n,
    authorityOwnerGeneration: 11n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 3n,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'mesh-a', rid: 'source' },
      descriptorLifecycleGeneration: 2n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date()
  };
  const location = actorJoinLocationStore(sourceAuthority, events);
  const registration = {
    spotNodes: new Map([['mesh-a', {
      actorFactoryRegistrations: {
        Player: { implementation: class Player {}, relocation: { kind: 'recreate' } }
      }
    }]])
  };
  const sourceState = {
    actorId,
    actorType: 'Player',
    actor: sourceActor,
    meshName: 'mesh-a',
    spotId: 'source-room',
    spotMembershipEpoch: 8n,
    nativeActorRef: { actorId, generation: 5n, nodeRid: 'source' },
    locationGeneration: 11n,
    ownerLeaseGeneration: 3n
  };
  const packets = ['B1', 'B2', 'D1'].map((label, index) =>
    actorJoinHandoffPacket(index, label));
  let releaseAccepted = () => {};
  const acceptedGate = options.holdAccepted === true
    ? new Promise<void>(resolve => { releaseAccepted = resolve; })
    : Promise.resolve();
  let releaseSourceLeave = () => {};
  const sourceLeaveGate = options.holdSourceLeave === true
    ? new Promise<void>(resolve => { releaseSourceLeave = resolve; })
    : Promise.resolve();
  let sourceLeaveDone!: () => void;
  const sourceLeaveIdle = new Promise<void>(resolve => { sourceLeaveDone = resolve; });
  const sourceCleanupRefs: unknown[] = [];
  const sourceLeaveSpotIds: unknown[] = [];
  const sourceActorManager = {
    async completeRelocationSource(_actorId: string, sourceRef: unknown) {
      sourceCleanupRefs.push(sourceRef);
      events.push('source:removed');
      sourceLeaveDone();
    }
  };
  const sourceActorTransfer = {
    async prepareMaintenanceSession() {
      return {
        target: {
          routerChannelId: 'session.route',
          targetNodeRid: 'session-owner',
          spotId: 'session-entry',
          sessionNodeRid: 'session-owner',
          sessionRid: 'session-1',
          bindingGeneration: 5n
        },
        handoffBacklog: packets.slice(0, 2),
        takeRelocationRelay: () => packets.slice(2),
        setReplayResults() {},
        async commit() { events.push('source:committed'); },
        async rollback() { events.push('source:rolled-back'); }
      };
    },
    async completeRelocationSourceLeave(_actor: unknown, sourceSpotId: unknown) {
      sourceLeaveSpotIds.push(sourceSpotId);
      events.push('source:onLeave:started');
      await sourceLeaveGate;
      events.push('source:onLeave:completed');
    }
  };

  let targetState: Record<string, any> | undefined;
  const targetActor = { context: { actorId, meshName: 'mesh-a' } };
  const targetActorManager = {
    published: 0,
    aborted: 0,
    async prepareRelocationActor() {
      events.push('restore:hidden');
      targetState = {
        actorId,
        nativeActorRef: { actorId, generation: 5n, nodeRid: 'target' },
        clearJoinedSpot() { this.spotId = undefined; },
        setJoinedSpot(spotId: unknown) { this.spotId = spotId; },
        setRemoteBoundSessionTarget(value: unknown) { this.remoteBoundSessionTarget = value; },
        setBoundSessionTransferTarget(value: unknown) { this.boundSessionTransferTarget = value; },
        setBoundSessionBindingGeneration(value: bigint) { this.bindingGeneration = value; }
      };
      return targetActor;
    },
    getState() { return targetState; },
    adoptCreatedAuthority() {
      const stage = [...((targetRuntime as any).targetStages as Map<string, any>).values()][0];
      const hidden = [...stage.staging.hidden.values()][0];
      const labels = hidden.replayPackets.map((packet: any) =>
        Buffer.from(packet.payload, 'base64').toString());
      events.push(`queue:merged:${labels.join(',')}`);
      events.push('route:closed');
    },
    publishRelocationActor() {
      this.published += 1;
      events.push('dispatch:open');
    },
    async abortRelocationActor() {
      this.aborted += 1;
      events.push('restore:aborted');
    }
  };
  const admissions = new ZLinkFormalRemoteActorAdmissionRegistry();
  admissions.begin({
    actorId,
    actorType: 'Player',
    actorRef: sourceActorRef as never,
    spotId: 'entry-target' as never,
    targetSpotGeneration: 6n,
    expectedMembershipEpoch: 8n,
    requestFingerprint: 'public-admission',
    transferId: relocationId,
    completionOperationId
  });
  admissions.complete(relocationId, {
    accepted: true,
    actorRef: sourceActorRef as never,
    deferredJoinRoot: { reference: 'accepted-root' } as never
  });
  assert.notEqual(`${completionOperationId.high.toString(16)}:${completionOperationId.low.toString(16)}`, relocationId);

  let sourceRuntime!: ZLinkHostServiceRelocationRuntime;
  let targetRuntime!: ZLinkHostServiceRelocationRuntime;
  let sourceSignal = new AbortController();
  const targetDeliveries: Promise<void>[] = [];
  const sourceDeliveries: Promise<void>[] = [];
  const deliveryErrors: unknown[] = [];
  const deliver = (
    runtime: ZLinkHostServiceRelocationRuntime,
    sourceNodeRid: string,
    bytes: Uint8Array,
    deliveries: Promise<void>[]
  ) => {
    const part = Message.from(bytes);
    const delivery = runtime.tryHandleControl('mesh-a', {
      kind: ReceiveKind.NodeSend,
      sourceNodeRid,
      parts: [part]
    } as never).then(() => undefined).catch(error => {
      deliveryErrors.push(error);
      if (runtime === targetRuntime && options.readyResults === undefined) {
        sourceSignal.abort(error);
      }
    }).finally(() => part.close());
    deliveries.push(delivery);
  };
  const sourceNode = {
    status: () => ({ routingId: 'source', lifecycleGeneration: 2n }),
    peers: () => [{ routingId: 'target', lifecycleGeneration: 6n, state: 3 }],
    sendToNode(_targetRid: string, bytes: Uint8Array) {
      const control = decodeServiceRelocationControlRequest(bytes);
      if (control !== undefined) {
        controlKinds.push(control.kind);
        if (control.kind === 'prepare') {
          prepareFingerprints.push(Buffer.from(bytes).toString('base64'));
        }
        if (control.kind === 'cutover' && options.dropCutover === true) {
          return SubmitResult.Ok;
        }
      }
      queueMicrotask(() => deliver(targetRuntime, 'source', Buffer.from(bytes), targetDeliveries));
      return SubmitResult.Ok;
    }
  };
  const targetNode = {
    status: () => ({ routingId: 'target', lifecycleGeneration: 6n }),
    peers: () => [{ routingId: 'source', lifecycleGeneration: 2n, state: 3 }],
    sendToNode(_sourceRid: string, bytes: Uint8Array) {
      const control = decodeServiceRelocationControlRequest(bytes);
      if (control?.kind === 'ready') {
        events.push('ready:submit');
        const queuedResult = options.readyResults?.shift();
        if (queuedResult !== undefined) return queuedResult;
        if (options.readyResult !== undefined) return options.readyResult;
      } else {
        events.push('sourceLeave:submit');
        if (options.sourceLeaveResult !== undefined) return options.sourceLeaveResult;
      }
      queueMicrotask(() => deliver(sourceRuntime, 'target', Buffer.from(bytes), sourceDeliveries));
      return SubmitResult.Ok;
    }
  };
  const targetSpotManager = {
    formalRemoteActorAdmissions: admissions,
    activations: { resolve: () => undefined },
    resolveRelocationActivation: () => undefined,
    options: {
      entryNodeRid: 'entry-target',
      async dispatchEntryActorJoin() {
        events.push('onJoined');
      },
      actorTransferRuntime: {
        async commitAndDeliverDeferredJoinAccepted() {
          events.push('accepted:started');
          await acceptedGate;
          events.push('accepted:completed');
        }
      }
    },
    finalizeActorJoinRelocation:
      DefaultZLinkSpotManager.prototype.finalizeActorJoinRelocation
  };
  const common = {
    registration,
    locationStore: () => location,
    liveDescriptors: async () => [targetDescriptor],
    completions: () => undefined,
    spotNodeRuntime: () => undefined,
    boundSessionRelocation: undefined
  };
  sourceRuntime = new ZLinkHostServiceRelocationRuntime({
    ...common,
    currentOwner: () => ({ ownerId: 'source-owner', leaseGeneration: 3n }),
    localDescriptor: () => ({ rid: 'source', lifecycleGeneration: 2n }),
    meshNode: () => sourceNode,
    spotManager: () => undefined,
    actorManager: () => sourceActorManager,
    actorTransfer: sourceActorTransfer
  } as never);
  targetRuntime = new ZLinkHostServiceRelocationRuntime({
    ...common,
    currentOwner: () => ({ ownerId: 'target-owner', leaseGeneration: 14n }),
    localDescriptor: () => targetDescriptor,
    meshNode: () => targetNode,
    spotManager: () => targetSpotManager,
    spotNodeRuntime: () => ({
      async dispatchEntryActorPacket(_actorId: string, parts: readonly Message[]) {
        events.push(`replay:${Buffer.from(parts[1]!.data()).toString()}`);
      }
    }),
    actorManager: () => targetActorManager,
    actorTransfer: {
      async publishRoutedActorOwnership() {
        assert.notEqual(targetState?.remoteBoundSessionTarget, undefined);
        events.push('command44');
      }
    }
  } as never);

  const relocate = async (advertisedReceiveChunkLimitBytes?: number) => {
    sourceSignal = new AbortController();
    const result = await sourceRuntime.relocateActorJoin({
      meshName: 'mesh-a',
      actor: sourceActor as never,
      state: sourceState as never,
      target: {
        routerChannelId: 'mesh-a',
        targetNodeRid: 'target' as never,
        spotId: 'entry-target' as never,
        spotKind: ZLinkSpotKind.Entry,
        targetSpotGeneration: 6n,
        targetNodeGeneration: 6n
      },
      relocationId,
      completionOperationId: `${completionOperationId.high.toString(16)}:${completionOperationId.low.toString(16)}`,
      advertisedReceiveChunkLimitBytes,
      signal: sourceSignal.signal
    });
    // The public Join owner installs the target ref before the one-way source
    // cleanup terminal arrives. Source cleanup must retain its original ref.
    (sourceState as any).nativeActorRef = result.actorRef;
    (sourceState as any).spotId = 'entry-target';
    return result;
  };
  return {
    events,
    controlKinds,
    prepareFingerprints,
    location,
    targetActorManager,
    relocate,
    releaseAccepted,
    releaseSourceLeave,
    sourceLeaveIdle: () => sourceLeaveIdle,
    sourceCleanupRefs,
    sourceLeaveSpotIds,
    sourceProfileCount: () =>
      ((sourceRuntime as any).sourceActorJoinProfiles as Map<string, unknown>).size,
    completeSourceCleanup: () => sourceRuntime.completeActorJoinSourceCleanup(actorId),
    async targetIdle() {
      await waitUntil(() => (targetRuntime as any).targetStages.size === 0);
      await Promise.all(targetDeliveries);
      assert.deepEqual(deliveryErrors, []);
    },
    async deliverCutoverAgain() {
      const prepare = controlKinds;
      assert.equal(prepare[0], 'prepare');
      const targetTerminal = (targetRuntime as any).terminalTargets;
      assert.notEqual(targetTerminal, undefined);
      const wire = {
        kind: 'cutover',
        relocation: { high: 0n, low: 145n },
        targetAttemptGeneration: 1n,
        coordinator,
        senderRole: 'source',
        object: {
          kind: 'actor',
          actorId,
          objectGeneration: 5n,
          expectedAuthorityOwnerGeneration: 11n
        },
        boundaryRecordCount: 0n,
        boundaryChecksumCrc32c: 0
      } as const;
      await dispatchRelocationControl(targetRuntime, 'source', wire as never);
    },
    async deliverSourceLeaveAgain() {
      const part = Message.from(Buffer.from(JSON.stringify({
        packetName: '__zlink.actor.source_leave.terminal',
        transferId: relocationId,
        actorId,
        succeeded: true
      })));
      try {
        return await sourceRuntime.tryHandleControl('mesh-a', {
          kind: ReceiveKind.NodeSend,
          sourceNodeRid: 'target',
          parts: [part]
        } as never);
      } finally {
        part.close();
      }
    },
    targetStageCount: () => (targetRuntime as any).targetStages.size as number,
    clearDeliveryErrors: () => { deliveryErrors.length = 0; },
    async dispose() {
      releaseAccepted();
      releaseSourceLeave();
      admissions.delete(relocationId);
      await Promise.all([...targetDeliveries, ...sourceDeliveries]);
      await sourceRuntime.dispose();
      await targetRuntime.dispose();
    }
  };
}

function actorJoinLocationStore(initial: ZLinkAuthoritySnapshot, events: string[]) {
  let current = initial;
  let prepared: any;
  let commits = 0;
  let aborts = 0;
  return {
    get commits() { return commits; },
    get aborts() { return aborts; },
    async readAuthority() { return current; },
    async prepareAggregate(request: any) {
      prepared = request;
      return {
        kind: 'prepared',
        fence: {
          aggregateId: request.aggregateId,
          aggregateGeneration: request.aggregateGeneration
        }
      };
    },
    async commitAggregate() {
      commits += 1;
      events.push('cas');
      const participant = prepared.participants[0];
      current = {
        ...current,
        storeVersion: { value: `target-v${commits + 1}` } as never,
        payload: Buffer.from(participant.authorityPayload),
        authorityOwnerGeneration: current.authorityOwnerGeneration + 1n,
        ownerId: prepared.targetOwner.ownerId,
        ownerLeaseGeneration: prepared.targetOwner.leaseGeneration,
        allocation: {
          ...current.allocation,
          descriptor: prepared.targetDescriptor,
          descriptorLifecycleGeneration: prepared.targetDescriptorLifecycleGeneration
        }
      };
      return { kind: 'committed' };
    },
    async abortAggregate() {
      aborts += 1;
      return { kind: 'aborted' };
    },
    async compareExchangeAuthority(_key: unknown, _expected: unknown, mutation: any) {
      current = {
        ...current,
        storeVersion: { value: `normalized-v${commits + 1}` } as never,
        payload: Buffer.from(mutation.payload)
      };
      return { ...current, kind: 'stored' };
    },
    async readOwnerLease() { return { kind: 'missing', storeNow: new Date() }; }
  };
}

function actorJoinHandoffPacket(index: number, label: string) {
  const parts = [Message.from(`header:${label}`), Message.from(label)];
  try {
    const messageFollowContext = createInitialActorMessageFollowContext({
      actorRef: {
        actorId: 'actor-host-profile',
        objectGeneration: 5n,
        meshName: 'mesh-a',
        nodeRid: 'source'
      },
      ownerId: 'source-owner',
      ownerLeaseGeneration: 3n,
      ownerNodeGeneration: 2n,
      authorityOwnerGeneration: 11n
    } as never, parts, false);
    return {
      index,
      header: Buffer.from(parts[0]!.data()).toString('base64'),
      payload: Buffer.from(parts[1]!.data()).toString('base64'),
      returnResponse: false,
      messageFollowContext
    };
  } finally {
    parts.forEach(part => part.close());
  }
}

async function dispatchRelocationControl(
  runtime: ZLinkHostServiceRelocationRuntime,
  sourceNodeRid: string,
  control: ServiceMaintenanceRelocationControl
): Promise<void> {
  const part = Message.from(encodeServiceRelocationControlRequest(control));
  try {
    assert.equal(await runtime.tryHandleControl('mesh-a', {
      kind: ReceiveKind.NodeSend,
      sourceNodeRid,
      parts: [part]
    } as never), true);
  } finally {
    part.close();
  }
}

async function waitUntil(predicate: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    if (predicate()) return;
    await new Promise<void>(resolve => setTimeout(resolve, 5));
  }
  assert.fail('condition did not become true');
}
