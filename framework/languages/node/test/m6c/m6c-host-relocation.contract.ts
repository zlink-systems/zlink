import { MessageChannel } from 'node:worker_threads';
import { ZLinkSpotActivation } from '../../packages/framework/src/runtime/spots/spot-activation-state';
import { ZLinkSpotActorMembership } from '../../packages/framework/src/runtime/spots/spot-actor-membership';
import { resolveActorIngress } from '../../packages/framework/src/runtime/spots/spot-actor-ingress-admission';
import { ZLinkActorRuntimeState } from '../../packages/framework/src/runtime/actors/actor-runtime-state';
import { ZLinkSpotSerialExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-executor';
import { ZLinkSpotSerialTurnExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-turn-executor';
import { ZLinkUserSpotExecutionMode } from '../../packages/framework/src/contracts';
import { inspectRuntime } from '../../packages/framework/src/runtime/internal-observation';
import { ZLinkSpotActorPacketDispatch } from '../../packages/framework/src/runtime/spots/spot-actor-packet-dispatch';
import {
  encodeStreamHeader,
  ZLinkStreamMessageKind,
  ZLinkStreamHeaderFlags,
  ZLinkStreamCodec
} from '../../packages/framework/src/runtime/streams/protocol';
import type { RawServiceMeshRuntime } from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import { setImmediate as turn } from 'node:timers/promises';
import { requestDurableService } from '../../packages/framework/src/runtime/foundation/durable-service-request';
import { ZLinkBackendResultError } from '../../packages/framework/src/runtime/backend/runtime-values';
import assert from 'node:assert/strict';
import { test, afterEach } from 'node:test';
import { RequestRuntime } from '../../packages/framework/src/runtime/foundation/request-runtime';
import type { PendingOperation } from '../../packages/framework/src/runtime/foundation/operation-registry';
import { frameworkErrorMappingFixture } from './framework-error-mapping-fixture';
import { Message, RequestResult, SubmitResult } from '@zlink-systems/zlink';
import { ZLinkSpotKind } from '../../packages/framework/src/contracts';
import type { ZLinkAuthoritySnapshot } from '../../packages/framework/src/runtime/locations/internal-location-contracts';
import { ZLinkInMemoryLocationStore } from '../../packages/framework/src/runtime/locations/in-memory-location-store';
import {
  createServiceRelocationId,
  decodeQueuedHandoffPacket,
  encodeHandoffQueuedMessages,
  relocationFailedFailureCode,
  ServiceRelocationDataLostError,
  ZLinkHostServiceRelocationRuntime
} from '../../packages/framework/src/runtime/host/service-relocation-host-runtime';
import {
  decodeServiceRelocationControlRequest,
  decodeServiceRelocationControlResponse,
  encodeServiceRelocationControlRequest,
  encodeServiceRelocationControlResponse
} from '../../packages/framework/src/runtime/host/service-relocation-control';
import {
  decodeSessionRelocationRoute,
  decodeSessionRelocationSealed,
  encodeSessionRelocationRoute,
  encodeSessionRelocationSeal,
  type ServiceMaintenanceRelocationControl,
  type ServiceMaintenanceRelocationCutover,
  type ServiceMaintenanceRelocationData,
  type ServiceMaintenanceRelocationFailed,
  type ServiceMaintenanceRelocationPrepare,
  type ServiceMaintenanceRelocationReady,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';
import {
  crc32c,
  ServiceRelocationAuthorityPayloadCodec
} from '../../packages/framework/src/runtime/foundation/service-relocation-runtime';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import { ZLinkActorTransferRuntime } from '../../packages/framework/src/runtime/host/actor-transfer-runtime';
import { ZLinkActorSerialExecutor } from '../../packages/framework/src/runtime/actors/actor-mailbox';
import { ZLinkActorSessionBindingRegistry } from '../../packages/framework/src/runtime/streams/actor-session-binding-registry';
import { ZLinkBoundSessionService } from '../../packages/framework/src/runtime/streams/bound-session-service';
import { encodeActorAuthorityIdentity } from '../../packages/framework/src/runtime/actors/actor-authority-publication';
import { ZLinkActorHandoffCoordinator } from '../../packages/framework/src/runtime/actors/actor-handoff';
import {
  attachActorMessageFollowContext,
  createInitialActorMessageFollowContext
} from '../../packages/framework/src/runtime/actors/actor-message-follow-context';
import { ReceiveKind } from '../../packages/framework/src/runtime/foundation/service-runtime-contracts';
import {
  ServiceStatefulRuntime,
  type ServiceStatefulMailboxData
} from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import {
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
  SERVICE_WIRE_REQUIRED_CAPABILITY
} from '../../packages/framework/src/runtime/foundation/service-wire-constants.generated';
import type { CanonicalActorJoinRecovery } from '../../packages/framework/src/runtime/foundation/actor-join-recovery-codec';
import { DefaultZLinkSpotManager } from '../../packages/framework/src/runtime/spots';
import { ZLinkActivationAdmission } from '../../packages/framework/src/runtime/activation-admission';
import {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} from '../../packages/framework/src/runtime/execution';
import { ZLinkRuntimeMetrics } from '../../packages/framework/src/runtime/diagnostics/runtime-metrics';
import { ZLinkFormalRemoteActorAdmissionRegistry } from '../../packages/framework/src/runtime/spots/formal-remote-actor-admission-registry';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';
import { ZLINK_INTERNAL_RELOCATION_INTEGRITY_FAULT_GATE } from '../../packages/framework/src/runtime/host/relocation-integrity-fault-gate';

const fixtureRequests = new Set<RequestRuntime>();
function fixtureRequestRuntime(): RequestRuntime {
  const requests = new RequestRuntime();
  fixtureRequests.add(requests);
  return requests;
}
afterEach(() => {
  for (const requests of fixtureRequests) requests.close();
  fixtureRequests.clear();
});

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

function createRuntimeMetricCollector() {
  const records: Array<{
    readonly name: string;
    readonly value: number;
    readonly attributes?: Readonly<Record<string, string>>;
  }> = [];
  const instrument = (name: string) => ({
    add(value: number, attributes?: Readonly<Record<string, string>>) {
      records.push({ name, value, attributes });
    },
    record(value: number, attributes?: Readonly<Record<string, string>>) {
      records.push({ name, value, attributes });
    }
  });
  const provider = {
    getMeter() {
      return {
        createCounter: (name: string) => instrument(name),
        createUpDownCounter: (name: string) => instrument(name),
        createHistogram: (name: string) => instrument(name),
        createObservableCounter: () => ({ addCallback() {} }),
        createObservableGauge: () => ({ addCallback() {} })
      };
    }
  };
  return { provider, records };
}

function actorJoinApplicationJobOwner() {
  const permit = {
    markApplicationQueued() {},
    detachForHandlerTurn() {},
    releaseBeforeHandler() {},
    releaseAfterInternalProcessing() {}
  };
  return {
    takeInitial: () => permit,
    close() {}
  };
}

test('relocation identity retries zero and local collisions with all 128 entropy bits', () => {
  const zero = Buffer.alloc(16);
  const collision = Buffer.from('00112233445566778899aabbccddeeff', 'hex');
  const accepted = Buffer.from('ffeeddccbbaa99887766554433221100', 'hex');
  const entropy = [zero, collision, accepted];
  const observed: string[] = [];
  const collisionId = '00112233-4455-6677-8899-aabbccddeeff';
  const acceptedId = 'ffeeddcc-bbaa-9988-7766-554433221100';

  const id = createServiceRelocationId(
    (candidate) => {
      observed.push(candidate);
      return candidate === collisionId;
    },
    (size) => {
      assert.equal(size, 16);
      return entropy.shift()!;
    }
  );

  assert.equal(id, acceptedId);
  assert.deepEqual(observed, [collisionId, acceptedId]);
  assert.equal(entropy.length, 0);
});

test('Actor relocation queue preserves Message Follow bigint origin fields', () => {
  const packet = {
    ...actorJoinHandoffPacket(0, 'message-follow-origin'),
    messageFollowOrigin: {
      sourceNodeRid: 'source-node',
      originalOperation: {
        high: 0xffff_ffff_ffff_ffffn,
        low: 7n
      },
      originalReplyRouteId: 0xffff_ffff_ffff_fffen
    }
  };
  const queued = encodeHandoffQueuedMessages([packet]);
  assert.equal(queued.length, 1);
  assert.deepEqual(
    decodeQueuedHandoffPacket(queued[0]!).messageFollowOrigin,
    packet.messageFollowOrigin
  );
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
  await registry.bind(context, actor, 'binding-token');
  await registry.sealRelocation(
    {
      actorId: actor.actorId,
      actorGeneration: 7n,
      bindingGeneration: 3n,
      sessionIdentity: 'session-1',
      actorNodeRid: 'source-node',
      actorNodeGeneration: 2n,
      sealId: 'relocation-1'
    },
    {
      objectGeneration: 7n,
      bindingGeneration: 3n
    }
  );

  const snapshot = (await registry.relocationSnapshot(actor.actorId, 'relocation-1'))!;
  assert.equal('actorOwnershipGeneration' in snapshot, false);
  assert.equal('ownerLeaseGeneration' in snapshot, false);
  await registry.applyRelocation(actor.actorId, 'relocation-1', 'route-fingerprint', async () => {
    assert.equal(await registry.abortSeal(actor.actorId, 'relocation-1'), true);
  });
  await registry.observeRelocationTerminal(actor.actorId, 'relocation-1', 'route-fingerprint');
});

test('pre-cutover rollback restores the source queue and state before one-way Session abort', async () => {
  const events: string[] = [];
  const actor = { context: { actorId: 'actor-rollback-order', meshName: 'mesh-a' } };
  let remoteTarget = {
    routerChannelId: 'mesh-a',
    targetNodeRid: 'session-owner',
    sessionNodeRid: 'session-owner',
    sessionRid: 'session-1',
    sessionOwnerNodeGeneration: 4n,
    sessionOwnerId: 'session-owner-id',
    sessionOwnerLeaseGeneration: 8n,
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
    get boundSession() {
      return remoteTarget;
    },
    beginMove() {
      events.push('move-begun');
    },
    endMove() {
      events.push('state-restored');
    }
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
      async dispatchRoutedActorPacket(_origin: 'local' | 'remote') {
        return { handled: true };
      }
    }),
    actorManager: () => undefined,
    primaryMeshNode: () => ({
      status: () => ({ routingId: 'source-node', lifecycleGeneration: 2n })
    }),
    async notifyEntrySpotActorLeft() {},
    locationLifecycle: () => undefined,
    actorHandoff: {
      begin() {},
      sealConnectionBoundIngress() {},
      snapshot() {
        return [];
      },
      takeRelocationRelay() {
        return [];
      },
      admitCanceledPrefix(
        _actorId: string,
        _queue: unknown,
        _preparation: unknown,
        start: Promise<void>
      ) {
        events.push('queue-restored');
        return { terminal: start.then(() => undefined) };
      }
    },
    authorityStore: () => ({ readAuthority: async () => authority }),
    relocationStore: () => undefined,
    sessionRelocationWire: () => ({
      async requestSessionRelocationSeal(
        _meshName: string,
        _targetNodeRid: unknown,
        seal: ServiceSessionRelocationSeal
      ) {
        events.push('session-sealed');
        return {
          relocation: seal.relocation,
          coordinator: seal.coordinator,
          actor: seal.actor,
          session: seal.session,
          sealResult: 'sealed'
        } satisfies ServiceSessionRelocationSealed;
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
    { high: 7n, low: 9n },
    { rid: 'target', lifecycleGeneration: 4n }
  );
  await prepared.rollback();
  await Promise.resolve();

  // Relocation 04 §4.1: the Session seal precedes the source dispatch stop.
  assert.deepEqual(events, [
    'session-sealed',
    'move-begun',
    'queue-restored',
    'state-restored',
    'session-abort'
  ]);
});

test('Session relocation is exact 42-to-43 and command 44 is one-way', async () => {
  const seal: ServiceSessionRelocationSeal = {
    targetNodeRid: 'target',
    targetNodeGeneration: 4n,
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
    sealResult: 'sealed',
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
      targetNodeGeneration: 6n,
      targetOwnerLeaseGeneration: 7n
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
    sealResult: 'sealed',
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
  let rejectRouteSubmit = false;
  let ownerAvailable = true;
  const rejectedControls: Array<{ outcome: string; errorReason: string }> = [];
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    currentOwner: () =>
      ownerAvailable ? { ownerId: 'session-owner-id', leaseGeneration: 8n } : undefined,
    messageFlow: () => ({
      accepts: () => true,
      trace: (event: { outcome: string; errorReason: string }) => rejectedControls.push(event)
    }),
    meshNode: () => ({
      status: () => ({ routingId: 'session-owner', lifecycleGeneration: 4n }),
      peers: () => [
        { routingId: 'source', lifecycleGeneration: 2n, state: 3 },
        { routingId: 'target', lifecycleGeneration: 6n, state: 3 }
      ],
      sendInfrastructureControl: (targetRid: string, bytes: Uint8Array) => {
        sent.push({ target: targetRid, bytes: Buffer.from(bytes) });
        return rejectRouteSubmit && targetRid === 'target'
          ? SubmitResult.NotConnected
          : SubmitResult.Ok;
      }
    }),
    boundSessionRelocation: {
      receiveSeal: async (
        value: ServiceSessionRelocationSeal,
        _signal?: AbortSignal,
        onInstalled?: () => void
      ) => {
        onInstalled?.();
        received.push('seal');
        if (received.filter((entry) => entry === 'seal').length === 1) {
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
        if (received.filter((entry) => entry === 'route').length === 1) {
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
    const pending = runtime.requestSessionRelocationSeal('mesh-a', 'session-owner', selfSeal);
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

    // A remote command 44 rejection is terminal for this one-way attempt:
    // it neither retries the identical control nor changes the committed
    // relocation terminal. The sealed Session performs its own timeout
    // cleanup; later recovery requires reconnect and explicit bind.
    rejectRouteSubmit = true;
    await runtime.sendSessionRelocationRoute('mesh-a', 'target', route);
    assert.equal(sent.length, 2, 'rejected command 44 has exactly one submit');
    assert.equal(sent[1]!.target, 'target');
    assert.deepEqual(decodeSessionRelocationRoute(sent[1]!.bytes), {
      ...route,
      actor: { ...route.actor, nodeRid: '' }
    });
    ownerAvailable = false;
    await assert.rejects(
      dispatch('source', encodeSessionRelocationSeal(seal)),
      (error: unknown) =>
        error instanceof ZLinkFrameworkException &&
        error.kind === ZLinkFrameworkErrorKind.Unavailable
    );
    assert.equal(sent.length, 2, 'a rejected command 42 must not send command 43');
    assert.equal(rejectedControls.length, 1);
    assert.equal(rejectedControls[0]!.outcome, 'dropped');
    assert.equal(rejectedControls[0]!.errorReason, 'location_unavailable');
  } finally {
    await runtime.dispose();
  }
});

test('exact duplicate Prepare shares restore while Data and Cutover stay one-way', async () => {
  const harness = createActorJoinHostHarness({ dropCutover: true });
  harness.location.liveLeases.set('source-owner', 3n);
  let preparations = 0;
  const prepareAggregate = harness.location.prepareAggregate.bind(harness.location);
  harness.location.prepareAggregate = async (request) => {
    preparations++;
    return prepareAggregate(request);
  };
  let release!: () => void, restoreEntered!: () => void;
  const heldRestore = new Promise<void>((resolve) => {
    release = resolve;
  });
  const restoring = new Promise<void>((resolve) => {
    restoreEntered = resolve;
  });
  let materializations = 0;
  const restore = harness.targetActorManager.prepareRelocationActor.bind(
    harness.targetActorManager
  );
  harness.targetActorManager.prepareRelocationActor = async (...args) => {
    materializations++;
    restoreEntered();
    await heldRestore;
    return restore(...args);
  };
  const relocation = harness.relocate();
  void relocation.catch(() => undefined);
  try {
    await restoring;
    const replies = await harness.deliverPrepareAgain(undefined, false);
    const foreignReplies = await harness.deliverPrepareAgain(4n, false);
    const conflicts = await harness.deliverPrepareAgain(undefined, true, true);
    const conflict = decodeServiceRelocationControlResponse(conflicts[0]!);
    assert.equal(conflict.kind, 'failed');
    if (conflict.kind === 'failed') assert.equal(conflict.failureCode, 35);
    await turn();
    assert.equal(materializations, 1);
    assert.equal(replies.length, 0, 'duplicate awaits the same in-flight restore');
    assert.equal(
      foreignReplies.length,
      0,
      'different exact identity is discarded (relocation §4.2)'
    );
    assert.equal(harness.readyReplies.length, 0);
    release();
    await waitUntil(() => replies.length === 1 && harness.controlKinds.includes('cutover'));
    assert.equal(materializations, 1);
    assert.equal(harness.readyReplies.length, 1);
    assert.deepEqual(
      replies[0],
      harness.readyReplies[0],
      'both exact receipts share the same validated READY'
    );
    assert.equal(foreignReplies.length, 0);
    assert.equal(harness.targetStageCount(), 1);
    assert.equal(replies.length, 1);
    assert.equal(decodeServiceRelocationControlResponse(replies[0]!).kind, 'ready');
    assert.equal(harness.events.filter((value) => value === 'restore:hidden').length, 1);
    assert.equal(preparations, 1, 'duplicate receipt cannot prepare another reservation');
    assert.equal(harness.targetStageCount(), 1);
    assert.equal(harness.controlKinds.filter((kind) => kind === 'data').length, 1);
    assert.equal(harness.controlKinds.filter((kind) => kind === 'cutover').length, 1);
    assert.equal(
      harness.events.filter((value) => value === 'ready:submit').length,
      1,
      'one-way DATA/CUTOVER have no reply callback'
    );
    assert.equal(harness.prepareFingerprints.length, 1);
    await harness.reachRestoreDeadline();
    await assert.rejects(relocation, /Preserve fence/);
    assert.equal(harness.location.preserves, 1);
  } finally {
    release();
    await harness.dispose();
  }
});

for (const restoreOutcome of ['ready', 'failed'] as const) {
  test(`Prepare manifest conflict preserves the original ${restoreOutcome} replay`, async () => {
    const prepare: ServiceMaintenanceRelocationPrepare = {
      kind: 'prepare',
      deadlineUnixMs: 1700000000000n,
      relocation: { high: 107n, low: 109n },
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
      applicationVersion: 4n
    };
    const sent: Buffer[] = [];
    let prepareCalls = 0;
    let readySubmitted = false;
    let release!: () => void;
    const held = new Promise<void>((resolve) => {
      release = resolve;
    });
    const runtime = new ZLinkHostServiceRelocationRuntime({
      requests: fixtureRequestRuntime(),
      meshNode: () => ({})
    } as never);
    const internals = runtime as unknown as {
      handlePrepareControl: () => Promise<ServiceMaintenanceRelocationReady>;
    };
    internals.handlePrepareControl = async () => {
      prepareCalls += 1;
      await held;
      if (restoreOutcome === 'failed') {
        throw new ServiceRelocationDataLostError('restore manifest failed');
      }
      return {
        kind: 'ready',
        relocation: prepare.relocation,
        targetAttemptGeneration: prepare.targetAttemptGeneration,
        coordinator,
        target,
        object,
        senderRole: 'target'
      };
    };
    const dispatch = async (request: ServiceMaintenanceRelocationPrepare) => {
      const part = Message.from(encodeServiceRelocationControlRequest(request));
      try {
        return await runtime.tryHandleControl('mesh-a', {
          kind: ReceiveKind.NodeRequest,
          sourceNodeRid: 'source',
          parts: [part],
          reply: (reply: Uint8Array | readonly Uint8Array[]) => {
            const bytes = Buffer.from(Array.isArray(reply) ? reply[0]! : reply);
            sent.push(bytes);
            if (decodeServiceRelocationControlResponse(bytes).kind === 'ready' && !readySubmitted) {
              readySubmitted = true;
              return SubmitResult.NotConnected;
            }
            return SubmitResult.Ok;
          }
        } as never);
      } finally {
        part.close();
      }
    };
    try {
      await dispatch(prepare);
      await dispatch({ ...prepare, payloadChecksumCrc32c: 124 });
      await waitUntil(() => sent.length === 1);
      const conflict = decodeServiceRelocationControlResponse(sent[0]!);
      assert.equal(conflict.kind, 'failed');
      assert.equal((conflict as ServiceMaintenanceRelocationFailed).failureCode, 35);
      assert.equal(prepareCalls, 1, 'conflict must not restart the existing restore');
      release();
      await waitUntil(() => sent.length === 2);
      const originalTerminal = sent[1]!;
      assert.equal(decodeServiceRelocationControlResponse(originalTerminal).kind, restoreOutcome);
      await dispatch({ ...prepare, sourceNodeGeneration: 3n });
      assert.equal(sent.length, 2, 'stale identity must not change the terminal reply');
      await dispatch(prepare);
      assert.equal(sent.length, 3);
      assert.deepEqual(sent[2], originalTerminal, 'the accepted restore outcome must still replay');
      assert.equal(prepareCalls, 1);
    } finally {
      release();
      await runtime.dispose();
    }
  });
}

test('the integrity fault gate fails the original Prepare with replayable DataLost', async () => {
  const prepare: ServiceMaintenanceRelocationPrepare = {
    kind: 'prepare',
    deadlineUnixMs: 1700000000000n,
    relocation: { high: 127n, low: 131n },
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
    applicationVersion: 4n
  };
  const gate = {
    consumeChecksumMismatch: () => false,
    consumeIdentityConflict: () => true
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    meshNode: () => ({}),
    providerResolver: {
      get: (token: unknown) =>
        token === ZLINK_INTERNAL_RELOCATION_INTEGRITY_FAULT_GATE ? gate : undefined
    }
  } as never);
  const sent: Buffer[] = [];
  const dispatch = async () => {
    const part = Message.from(encodeServiceRelocationControlRequest(prepare));
    try {
      await runtime.tryHandleControl('mesh-a', {
        kind: ReceiveKind.NodeRequest,
        sourceNodeRid: 'source',
        parts: [part],
        reply: (reply: Uint8Array | readonly Uint8Array[]) => {
          sent.push(Buffer.from(Array.isArray(reply) ? reply[0]! : reply));
          return SubmitResult.Ok;
        }
      } as never);
    } finally {
      part.close();
    }
  };
  try {
    await dispatch();
    await waitUntil(() => sent.length === 1);
    const terminal = decodeServiceRelocationControlResponse(sent[0]!);
    assert.equal(terminal.kind, 'failed');
    assert.equal((terminal as ServiceMaintenanceRelocationFailed).failureCode, 35);
    await dispatch();
    assert.equal(sent.length, 2);
    assert.deepEqual(sent[1], sent[0], 'fault terminal must replay without attempting Restore');
  } finally {
    await runtime.dispose();
  }
});

test('an explicit Failed(53) on the Prepare reply leg rejects promptly with its classified failure', async () => {
  for (const [failureCode, expectedKind] of [
    [35, ZLinkFrameworkErrorKind.DataLost],
    [17, ZLinkFrameworkErrorKind.InternalFailure]
  ] as const) {
    const harness = createActorJoinHostHarness({ explicitFailureCode: failureCode });
    try {
      const started = performance.now();
      await assert.rejects(harness.relocate(), (error: unknown) => {
        assert(error instanceof ZLinkFrameworkException);
        assert.equal(error.kind, expectedKind, `failureCode ${failureCode}`);
        return true;
      });
      assert(performance.now() - started < 1_000);
      assert(harness.events.includes('source:rolled-back'));
      assert.equal(harness.prepareFingerprints.length, 1);
      assert.equal(harness.location.preserves, 0);
      assert.equal(harness.location.commits, 0);
    } finally {
      await harness.dispose();
    }
  }
});

test('relocation failure codes use the shared error model fixture', () => {
  for (const row of frameworkErrorMappingFixture.send)
    assert.equal(
      relocationFailedFailureCode(
        new ZLinkFrameworkException(ZLinkFrameworkErrorKind[row.kind], 'fixture')
      ),
      row.codeOnlyFailureCode ?? row.failureCode,
      row.kind
    );
  assert.equal(relocationFailedFailureCode(new ServiceRelocationDataLostError('checksum')), 35);
  assert.equal(relocationFailedFailureCode(new Error('untyped restore failure')), 17);
});

test('cutover boundary reconciliation replaces a stale pre-reconnect span with the retransmitted whole batch', async () => {
  // Spec 28 §4.4: a retransmission after reconnect always resends the entire
  // boundary batch. If the target already buffered a partially received span
  // from before the disconnect, the retransmitted batch (declared by the
  // cutover's boundaryRecordCount/boundaryChecksumCrc32c) must replace it as
  // a whole rather than being appended after it.
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime()
  } as never);
  const internals = runtime as unknown as {
    reconcileBoundaryRelay: (
      stage: { boundaryRelay: ServiceMaintenanceRelocationData[] },
      request: ServiceMaintenanceRelocationCutover,
      stagingId: string
    ) => void;
  };
  const record = (label: string): ServiceMaintenanceRelocationData =>
    ({
      kind: 'data',
      relocation: { high: 0n, low: 1n },
      targetAttemptGeneration: 1n,
      coordinator,
      senderRole: 'source',
      object,
      frozenRecord: { canonicalBytes: Buffer.from(label) }
    }) as unknown as ServiceMaintenanceRelocationData;

  try {
    const stale = [record('stale-0'), record('stale-1')];
    const retransmitted = [record('whole-0'), record('whole-1'), record('whole-2')];
    const stage = { boundaryRelay: [...stale, ...retransmitted] };
    const boundaryChecksumCrc32c = crc32c(
      Buffer.concat(retransmitted.map((value) => value.frozenRecord.canonicalBytes))
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
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime()
  } as never);
  const internals = runtime as unknown as {
    reconcileBoundaryRelay: (
      stage: { boundaryRelay: ServiceMaintenanceRelocationData[] },
      request: ServiceMaintenanceRelocationCutover,
      stagingId: string
    ) => void;
  };
  const record = (label: string): ServiceMaintenanceRelocationData =>
    ({
      kind: 'data',
      relocation: { high: 0n, low: 1n },
      targetAttemptGeneration: 1n,
      coordinator,
      senderRole: 'source',
      object,
      frozenRecord: { canonicalBytes: Buffer.from(label) }
    }) as unknown as ServiceMaintenanceRelocationData;

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

function targetCasFixture() {
  const actorKey = encodeAuthorityKey('actor', object.actorId);
  const envelope = {
    aggregateId: '00000000-0000-0000-0000-000000000011',
    aggregateGeneration: 1n,
    participants: [
      {
        key: actorKey.value,
        objectKind: 'actor',
        stableType: 'Player',
        objectGeneration: object.objectGeneration,
        authorityOwnerGeneration: object.expectedAuthorityOwnerGeneration,
        applicationState: Buffer.alloc(0),
        boundSessionState: Buffer.alloc(0),
        queuedMessages: [],
        timers: []
      }
    ],
    memberships: [
      {
        actorKey: actorKey.value,
        spotKey: encodeAuthorityKey('user_spot', 'entry-target').value,
        spotObjectGeneration: 6n,
        membershipEpoch: 1n
      }
    ]
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
    deadlineUnixMs: 1700000000000n,
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
    applicationVersion: 4n
  };
  const authorityPayload = Buffer.from('target-authority');
  const committedAuthority: ZLinkAuthoritySnapshot = {
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
  const commit = (
    runtime: ZLinkHostServiceRelocationRuntime
  ): Promise<ZLinkAuthoritySnapshot | undefined> => {
    return (
      runtime as unknown as {
        commitTargetReservation(
          stage: unknown,
          reservation: unknown
        ): Promise<ZLinkAuthoritySnapshot | undefined>;
      }
    ).commitTargetReservation(
      {
        offer: {
          prepare,
          prepareFingerprint: 'prepare',
          authenticatedSourceNodeRid: coordinator.nodeRid,
          envelope
        },
        // Only a verified cutover lets the target submit its CAS (spec 28 §4.4).
        cutoverReceived: true,
        boundaryRelay: [],
        staging: { primaryAuthorityKey: actorKey, envelope }
      },
      {
        prepared: {
          fence: {
            aggregateId: { value: envelope.aggregateId },
            aggregateGeneration: envelope.aggregateGeneration
          },
          plan: {
            envelope,
            participants: [
              {
                key: actorKey,
                expected,
                ownerTransition: 'newOwner',
                authorityPayload,
                membershipMutation: Buffer.from('membership')
              }
            ],
            targetDescriptor: { meshName: 'mesh-a', rid: target.nodeRid },
            targetDescriptorLifecycleGeneration: target.nodeGeneration,
            capacity: expected.allocation.capacity,
            targetOwner: {
              ownerId: target.ownerId,
              leaseGeneration: target.ownerLeaseGeneration
            }
          }
        }
      }
    );
  };
  return {
    envelope,
    expected,
    committedAuthority,
    // The settlement retry timer is unref'd like every runtime timer; the
    // test keeps its own event loop alive while the settlement runs.
    commit: async (runtime: ZLinkHostServiceRelocationRuntime) => {
      const keepAlive = setInterval(() => undefined, 1_000);
      try {
        return await commit(runtime);
      } finally {
        clearInterval(keepAlive);
      }
    }
  };
}

for (const operation of ['read', 'lease', 'preserve'] as const) {
  for (const error of [
    new TypeError('invalid source key'),
    new RangeError('invalid source bound')
  ]) {
    test(`source settlement propagates ${error.name} from Store ${operation}`, async () => {
      const fixture = targetCasFixture();
      let calls = 0;
      const runtime = new ZLinkHostServiceRelocationRuntime({
        requests: fixtureRequestRuntime(),
        locationStore: () => ({
          readAuthority: async () => {
            if (operation === 'read' && ++calls === 1) throw error;
            return calls > 0 && operation !== 'lease'
              ? fixture.committedAuthority
              : fixture.expected;
          },
          readOwnerLease: async () => {
            if (operation === 'lease' && ++calls === 1) throw error;
            return {
              kind: 'found',
              token: {
                ownerId: fixture.expected.ownerId,
                leaseGeneration: fixture.expected.ownerLeaseGeneration
              },
              leaseExpiresAt: new Date(Date.now() + 60_000),
              storeNow: new Date()
            };
          },
          compareExchangeAuthority: async () => {
            if (operation === 'preserve' && ++calls === 1) throw error;
            return { kind: 'stored' };
          }
        })
      } as never);
      const source = runtime as unknown as {
        settleSourceAuthority(...args: unknown[]): Promise<unknown>;
      };
      await assert.rejects(
        source.settleSourceAuthority(fixture.envelope, fixture.expected, target, () => true, {}),
        (observed) => observed === error
      );
      assert.equal(calls, 1);
    });
  }
}

test('source settlement retains its fence after a provider read failure', async () => {
  const fixture = targetCasFixture();
  let reads = 0;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    locationStore: () => ({
      readAuthority: async () => {
        if (++reads === 1) throw new Error('Store response lost');
        return fixture.committedAuthority;
      }
    })
  } as never);
  const source = runtime as unknown as {
    settleSourceAuthority(...args: unknown[]): Promise<unknown>;
  };
  assert.deepEqual(
    await source.settleSourceAuthority(fixture.envelope, fixture.expected, target, () => false, {}),
    {
      kind: 'target',
      followAuthorities: new Map([
        [fixture.envelope.participants[0].key, fixture.committedAuthority]
      ])
    }
  );
  assert.equal(reads, 2);
});

test('target-only CAS reconciles an unknown response to the exact committed owner', async () => {
  const fixture = targetCasFixture();
  let current = fixture.expected;
  let writes = 0;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    locationStore: () => ({
      commitAggregate: async () => {
        writes += 1;
        current = fixture.committedAuthority;
        throw new Error('commit response lost');
      },
      readAuthority: async () => current
    })
  } as never);
  const committed = await fixture.commit(runtime);

  assert.equal(writes, 1);
  assert.equal(committed?.ownerId, target.ownerId);
  assert.equal(committed?.authorityOwnerGeneration, 12n);
});

test('target CAS propagates public Store invalid key during reconciliation', async () => {
  const store = new ZLinkInMemoryLocationStore();
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    locationStore: () => store
  } as never);
  const read = runtime as unknown as {
    readAggregateForCommitRetry(prepared: unknown): Promise<unknown>;
  };
  await assert.rejects(
    read.readAggregateForCommitRetry({ plan: { participants: [{ key: { value: '' } }] } }),
    TypeError
  );
});

for (const validationError of [
  new TypeError('invalid aggregate'),
  new RangeError('invalid bound')
]) {
  for (const operation of ['commit', 'read', 'lease'] as const) {
    test(`target CAS propagates ${validationError.name} from Store ${operation}`, async () => {
      const fixture = targetCasFixture();
      let calls = 0;
      const runtime = new ZLinkHostServiceRelocationRuntime({
        requests: fixtureRequestRuntime(),
        locationStore: () => ({
          commitAggregate: async () => {
            if (operation === 'commit' && ++calls === 1) throw validationError;
            if (operation === 'lease') throw new Error('commit response lost');
            return { kind: 'stale' };
          },
          readAuthority: async () => {
            if (operation === 'read' && ++calls === 1) throw validationError;
            return fixture.expected;
          },
          readOwnerLease: async () => {
            if (operation === 'lease' && ++calls === 1) throw validationError;
            return { kind: 'missing', storeNow: new Date() };
          }
        })
      } as never);
      await assert.rejects(fixture.commit(runtime), (error) => error === validationError);
      assert.equal(calls, 1, 'caller validation cannot be retried as an unknown Store result');
    });
  }
}

test('target CAS resubmits indeterminate results with no deadline while the target lease is valid', async () => {
  const fixture = targetCasFixture();
  let current = fixture.expected;
  let writes = 0;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    locationStore: () => ({
      commitAggregate: async () => {
        writes += 1;
        if (writes < 3) throw new Error('commit response lost before the write');
        current = fixture.committedAuthority;
        return { kind: 'committed' };
      },
      readAuthority: async () => current,
      readOwnerLease: async (ownerId: string) => ({
        kind: 'found',
        token: { ownerId, leaseGeneration: target.ownerLeaseGeneration },
        leaseExpiresAt: new Date(Date.now() + 60_000),
        storeNow: new Date()
      })
    })
  } as never);
  const committed = await fixture.commit(runtime);
  assert.equal(writes, 3, 'the same NewOwner CAS is resubmitted, never abandoned');
  assert.equal(committed?.ownerId, target.ownerId);
});

test(
  'target CAS discards staging when the target lease ends before commit confirmation',
  { timeout: 5_000 },
  async () => {
    const fixture = targetCasFixture();
    let writes = 0;
    const runtime = new ZLinkHostServiceRelocationRuntime({
      requests: fixtureRequestRuntime(),
      locationStore: () => ({
        commitAggregate: async () => {
          writes += 1;
          throw new Error('commit result unknown');
        },
        readAuthority: async () => fixture.expected,
        readOwnerLease: async () => ({ kind: 'missing', storeNow: new Date() })
      })
    } as never);
    assert.equal(await fixture.commit(runtime), undefined);
    assert.equal(writes, 1);
  }
);

test(
  'target CAS discards staging on a definitive conflict that excludes its commit',
  { timeout: 5_000 },
  async () => {
    const fixture = targetCasFixture();
    let writes = 0;
    const runtime = new ZLinkHostServiceRelocationRuntime({
      requests: fixtureRequestRuntime(),
      locationStore: () => ({
        commitAggregate: async () => {
          writes += 1;
          return { kind: 'stale' };
        },
        readAuthority: async () => fixture.expected,
        readOwnerLease: async (ownerId: string) => ({
          kind: 'found',
          token: { ownerId, leaseGeneration: target.ownerLeaseGeneration },
          leaseExpiresAt: new Date(Date.now() + 60_000),
          storeNow: new Date()
        })
      })
    } as never);
    assert.equal(await fixture.commit(runtime), undefined);
    assert.equal(writes, 1);
  }
);

test('host relocation reports units settled on both sides as an authority error, all-source as a plain block', async () => {
  const run = async (committed: ReadonlySet<string>) => {
    const runtime = new ZLinkHostServiceRelocationRuntime({
      requests: fixtureRequestRuntime(),
      spotManager: () => ({ relocationActivations: () => [] }),
      actorManager: () => ({
        snapshotStates: () =>
          ['actor-a', 'actor-b'].map((actorId) => ({ actorId, actor: {}, meshName: 'mesh-a' }))
      })
    } as never);
    (runtime as any).relocateStandaloneActor = async (
      _mesh: string,
      state: { actorId: string }
    ) => {
      if (!committed.has(state.actorId)) {
        throw new Error(`Relocation of '${state.actorId}' ended by the source Preserve fence.`);
      }
    };
    return await runtime.relocateMesh('mesh-a');
  };
  await assert.rejects(
    run(new Set(['actor-a'])),
    (error: unknown) => error instanceof Error && error.name === 'ServiceRelocationAuthorityError'
  );
  await assert.rejects(
    run(new Set()),
    (error: unknown) =>
      error instanceof Error &&
      error.name !== 'ServiceRelocationAuthorityError' &&
      /Preserve fence/.test(error.message)
  );
  await run(new Set(['actor-a', 'actor-b']));
});

test('target-ready authority keeps aggregate, attempt and coordinator StoreVersion distinct', () => {
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime()
  } as never);
  const sourceAuthority = encodeActorAuthorityIdentity({
    actorType: 'Player',
    actor: {
      nodeRid: coordinator.nodeRid,
      actorId: object.actorId,
      objectGeneration: object.objectGeneration,
      meshName: 'mesh-a'
    },
    meshName: 'mesh-a',
    ownerNodeGeneration: coordinator.nodeGeneration,
    owner: {
      ownerId: coordinator.ownerId,
      leaseGeneration: coordinator.leaseGeneration
    },
    spotId: 'source-entry',
    spotGeneration: coordinator.nodeGeneration,
    spotKind: ZLinkSpotKind.Entry
  });
  const projected = (
    runtime as unknown as {
      authorityPayloadForPublication(
        payload: Uint8Array,
        publication: unknown,
        target: unknown
      ): Uint8Array;
    }
  ).authorityPayloadForPublication(
    sourceAuthority,
    {
      reference: 'root/1',
      checksumCrc32c: 0x0102_0304,
      aggregateId: '00000000-0000-0001-0000-000000000002',
      aggregateGeneration: 2n,
      inventoryDigest: '0'.repeat(64),
      targetOwnerId: target.ownerId,
      targetOwnerLeaseGeneration: target.ownerLeaseGeneration
    },
    {
      owner: {
        ownerId: target.ownerId,
        leaseGeneration: target.ownerLeaseGeneration
      },
      meshName: 'mesh-a',
      nodeRid: target.nodeRid,
      nodeGeneration: target.nodeGeneration,
      objectGeneration: object.objectGeneration,
      targetAttemptGeneration: 3n,
      coordinatorExpectedStoreVersion: 'source-v1',
      actorSpotId: 'target-entry',
      actorSpotGeneration: target.nodeGeneration,
      actorSpotKind: ZLinkSpotKind.Entry
    }
  );
  const publication = new ServiceRelocationAuthorityPayloadCodec().read(projected)!;

  assert.equal(publication.aggregateGeneration, 2n);
  assert.equal(publication.targetAttemptGeneration, 3n);
  assert.equal(publication.coordinatorExpectedStoreVersion, 'source-v1');
  assert.equal(publication.sourceNodeRid, coordinator.nodeRid);
  assert.equal(publication.targetNodeRid, target.nodeRid);
  assert.equal(publication.coordinatorOwnerId, target.ownerId);
});

test('ActorJoin target invalidates a previous-owner Actor route before lifecycle and preserves post-open relay order', async () => {
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
    requests: fixtureRequestRuntime(),
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
      },
      async sendInfrastructureControl() {
        return SubmitResult.Ok;
      }
    }),
    actorTransfer: {
      async publishRoutedActorOwnership() {
        events.push('command44');
      }
    },
    invalidateActorRoute: (actorId: string) => {
      assert.equal(actorId, 'actor-join');
      events.push('actorRoute:invalidated');
    }
  } as never);
  const internals = runtime as unknown as {
    commitTargetReservation: () => Promise<ZLinkAuthoritySnapshot>;
    targetReplyRelayCoordinator: () => unknown;
    clearTargetRelocationPublication: () => Promise<ZLinkAuthoritySnapshot>;
    finalizeTargetStage: (meshName: string, stagingId: string, stage: unknown) => Promise<void>;
  };
  internals.commitTargetReservation = async () => {
    events.push('cas');
    return authority;
  };
  internals.targetReplyRelayCoordinator = () => ({});
  internals.clearTargetRelocationPublication = async () => {
    events.push('publication:clear');
    return authority;
  };
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
      async publish() {
        events.push('route:closed');
      },
      async normalize() {
        events.push('queue:merged');
      },
      async openAdmission() {
        events.push('dispatch:open');
      }
    },
    staging: {
      envelope,
      hidden: new Map([[hidden.authorityKey, hidden]])
    },
    phase: 'ready',
    lane: Promise.resolve(),
    cutoverReceived: true,
    boundaryRelay: [],
    releaseJoinAdmission: () => undefined,
    releaseActivation: () => undefined
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
    'route:closed',
    'publication:clear',
    'queue:merged',
    'actorRoute:invalidated',
    'onJoined',
    'sourceLeave:submit',
    'sourceLeave:warning',
    'accepted',
    'command44',
    'dispatch:open'
  ]);
});

test('target admission opens after one publication-clear conflict and a later cleanup clears it', async () => {
  const events: string[] = [];
  const key = encodeAuthorityKey('actor', 'actor-clear-retry');
  const codec = new ServiceRelocationAuthorityPayloadCodec();
  const publication = {
    reference: 'relocation-root',
    checksumCrc32c: 7,
    aggregateId: '00000000-0000-0000-0000-000000000022',
    aggregateGeneration: 1n,
    inventoryDigest: '0'.repeat(64),
    targetOwnerId: target.ownerId,
    targetOwnerLeaseGeneration: target.ownerLeaseGeneration
  } as const;
  let current = {
    kind: 'snapshot',
    storeVersion: { value: 'target-v2' } as never,
    payload: codec.publish(Buffer.from('actor-state'), publication),
    objectGeneration: 5n,
    authorityOwnerGeneration: 12n,
    ownerId: target.ownerId,
    ownerLeaseGeneration: target.ownerLeaseGeneration,
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
  let conflictsRemaining = 1;
  let clearAttempts = 0;
  let actorAvailable = false;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    spotManager: () => undefined,
    locationStore: () => ({
      async readAuthority() {
        return current;
      },
      async compareExchangeAuthority(
        _key: unknown,
        _version: unknown,
        mutation: { payload: Uint8Array }
      ) {
        clearAttempts += 1;
        if (conflictsRemaining > 0) {
          conflictsRemaining -= 1;
          return { kind: 'conflict' };
        }
        current = {
          ...current,
          storeVersion: { value: `target-v${clearAttempts + 2}` } as never,
          payload: Buffer.from(mutation.payload)
        };
        return { kind: 'stored' };
      }
    }),
    metrics: {
      count(name: string) {
        events.push(`metric:${name}`);
      },
      duration() {}
    }
  } as never);
  const internals = runtime as unknown as {
    commitTargetReservation: () => Promise<ZLinkAuthoritySnapshot>;
    targetReplyRelayCoordinator: () => unknown;
    clearTargetRelocationPublication: (
      stage: unknown,
      authority: ZLinkAuthoritySnapshot
    ) => Promise<void>;
    finalizeTargetStage: (meshName: string, stagingId: string, stage: unknown) => Promise<void>;
  };
  internals.commitTargetReservation = async () => current;
  internals.targetReplyRelayCoordinator = () => ({});
  const envelope = {
    aggregateId: publication.aggregateId,
    aggregateGeneration: publication.aggregateGeneration,
    participants: [],
    memberships: []
  };
  const stage = {
    offer: { prepareFingerprint: 'clear-retry' },
    owner: {
      async normalize() {},
      async publish() {},
      async openAdmission() {
        actorAvailable = true;
      }
    },
    staging: {
      primaryAuthorityKey: key,
      envelope,
      hidden: new Map()
    },
    phase: 'ready',
    lane: Promise.resolve(),
    cutoverReceived: true,
    boundaryRelay: [],
    releaseJoinAdmission: () => undefined,
    releaseActivation: () => undefined
  };
  const originalWarn = console.warn;
  console.warn = (marker) => events.push(String(marker));
  try {
    await internals.finalizeTargetStage('mesh-a', 'clear-retry-stage', stage);
    assert.equal(
      actorAvailable,
      true,
      'the committed Actor must be dispatchable after clear conflicts'
    );
    assert.equal(stage.phase, 'open');
    // One CAS against the committed snapshot decides; a conflict is recorded,
    // not retried (the target owner is the only writer of that row).
    assert.equal(clearAttempts, 1);
    assert.deepEqual(events, [
      '[zlink.runtime.relocation.publication_clear_failed]',
      'metric:zlink.relocation.publication_clear_failed'
    ]);

    await internals.clearTargetRelocationPublication(stage, current);
    assert.equal(
      clearAttempts,
      2,
      'the retained publication must be clearable by follow-up cleanup'
    );
    assert.equal(codec.read(current.payload), undefined);
  } finally {
    console.warn = originalWarn;
    await runtime.dispose();
  }
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
    assert.deepEqual(
      harness.events.filter(
        (value) =>
          value === 'cas' ||
          value.startsWith('raw-authority:') ||
          value === 'route:closed' ||
          value.startsWith('queue:merged:') ||
          value === 'onJoined' ||
          value === 'sourceLeave:submit' ||
          value === 'accepted:completed' ||
          value === 'dispatch:open' ||
          value.startsWith('replay:') ||
          value === 'command44'
      ),
      [
        'cas',
        'raw-authority:12',
        'queue:merged:B1,B2,D1',
        'route:closed',
        'onJoined',
        'sourceLeave:submit',
        'accepted:completed',
        'command44',
        'dispatch:open',
        'replay:B1',
        'replay:B2',
        'replay:D1'
      ]
    );
    assert.equal(
      harness.targetAuthorityGeneration(),
      12n,
      'command 44 must observe the committed target authority in the native registry'
    );
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
    assert.deepEqual(
      harness.sourceCleanupRefs,
      [
        {
          actorId: 'actor-host-profile',
          generation: 5n,
          nodeRid: 'source'
        }
      ],
      'source cleanup must use the exact pre-relocation native authority ref'
    );
    assert.deepEqual(
      harness.sourceLeaveSpotIds,
      ['source-room'],
      'source leave must use the exact pre-relocation Spot identity'
    );
    const removals = harness.events.filter((value) => value === 'source:removed').length;
    assert.equal(await harness.deliverSourceLeaveAgain(), true);
    await Promise.resolve();
    assert.equal(
      harness.events.filter((value) => value === 'source:removed').length,
      removals,
      'duplicate source-leave terminal must be handled with zero mutation'
    );
  } finally {
    await harness.dispose();
  }
});

test('public ActorJoin relocation records one object metric from start through terminal result', async () => {
  const collector = createRuntimeMetricCollector();
  const metrics = new ZLinkRuntimeMetrics(collector.provider as never);
  const harness = createActorJoinHostHarness({ metrics });
  try {
    await harness.relocate();
    await harness.targetIdle();

    const started = collector.records.filter(
      (record) => record.name === 'zlink.relocation.started'
    );
    const completed = collector.records.filter(
      (record) => record.name === 'zlink.relocation.completed'
    );
    const duration = collector.records.filter(
      (record) => record.name === 'zlink.relocation.duration'
    );
    const bytes = collector.records.filter((record) => record.name === 'zlink.relocation.bytes');
    assert.equal(started.length, 1);
    assert.equal(completed.length, 1);
    assert.deepEqual(started[0]?.attributes, {
      mesh_name: 'mesh-a',
      object_kind: 'actor',
      policy: 'recreate'
    });
    assert.deepEqual(completed[0]?.attributes, {
      mesh_name: 'mesh-a',
      object_kind: 'actor',
      policy: 'recreate',
      outcome: 'completed'
    });
    assert.equal(duration.length, 1);
    assert.equal(bytes.length, 1);
    assert.ok(bytes[0]!.value > 0);
  } finally {
    await harness.dispose();
  }
});

test('canonical ActorJoin admission recovery crosses command 28 into command 40 without preloaded admission state', async () => {
  const harness = createActorJoinHostHarness({ canonicalRecovery: true });
  const prepare = harness.targetActorManager.prepareRelocationActor;
  harness.targetActorManager.prepareRelocationActor = async (...args) => {
    assert.equal(harness.canonicalAdmissionState(), 'restoring', 'PREPARE claims before factory');
    return await prepare(...args);
  };
  try {
    const relocated = await harness.relocate();
    await harness.targetIdle();

    assert.equal(String(relocated.actorRef.nodeRid), 'target');
    const recovery = harness.canonicalRecoveryIdentity();
    assert.notEqual(recovery, undefined);
    assert.equal(recovery!.request.sourceSpotId, 'source-room');
    assert.equal(
      harness.canonicalAdmissionActorNodeRid(),
      'source',
      'command 28 admission must retain the source ActorRef until target CAS'
    );
    assert.equal(
      recovery!.request.relocationAggregateId.replaceAll('-', ''),
      recovery!.request.handoffId,
      'canonical HandoffId must own the relocation aggregate'
    );
    assert.equal(recovery!.request.reservationToken, recovery!.request.handoffId);
    assert.equal(
      recovery!.request.reservedPayloadBytes,
      BigInt(64 * 1024 + 16 * 1024 * 1024 + Buffer.byteLength('canonical-request'))
    );
    assert.deepEqual(harness.controlKinds, ['prepare', 'state', 'data', 'cutover']);
    assert.deepEqual(
      harness.events.filter(
        (value) =>
          value === 'recovery:prepared' ||
          value === 'onJoined' ||
          value === 'accepted:completed' ||
          value.startsWith('queue:merged:') ||
          value.startsWith('replay:')
      ),
      [
        'recovery:prepared',
        'queue:merged:B1,B2,D1',
        'onJoined',
        'accepted:completed',
        'replay:B1',
        'replay:B2',
        'replay:D1'
      ]
    );
  } finally {
    await harness.dispose();
  }
});

test('canonical ActorJoin recovery retains admission and typed reply until attempt terminal', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const actorRef = {
    actorId: 'actor-canonical-reply-type',
    objectGeneration: 5n,
    meshName: 'mesh-a',
    nodeRid: 'source'
  };
  const handoffId = '00112233445566778899aabbccddeeff';
  const request = Buffer.from('canonical-request');
  const reply = Buffer.from('{"accepted":true}');
  const admissions = new ZLinkFormalRemoteActorAdmissionRegistry();
  admissions.begin({
    actorId: actorRef.actorId,
    actorType: 'Player',
    actorRef,
    spotId: 'room-target',
    targetSpotGeneration: 6n,
    expectedMembershipEpoch: 1n,
    requestFingerprint: request.toString('base64'),
    transferId: handoffId,
    sourceActorNodeRid: 'source'
  });
  admissions.complete(handoffId, {
    accepted: true,
    actorRef,
    reply,
    replyContentType: 'application/json'
  });
  let preparedContentType: string | undefined;
  const manager = {
    formalRemoteActorAdmissions: admissions,
    options: {
      actorTransferRuntime: {
        prepareDeferredJoinAccepted(
          _actorId: string,
          _operationId: unknown,
          _actor: unknown,
          _reply: Uint8Array,
          replyContentType?: string
        ) {
          preparedContentType = replyContentType;
          return { actor: actorRef };
        }
      }
    }
  };
  const recovery: CanonicalActorJoinRecovery = {
    source: {
      nodeRid: 'source',
      nodeGeneration: 2n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 3n
    },
    request: {
      actorId: actorRef.actorId,
      actorType: 'Player',
      handoffId,
      sourceSpotId: 'source-room',
      sourceNodeRid: Buffer.from('source'),
      actorGeneration: actorRef.objectGeneration,
      actorAuthorityOwnerGeneration: 11n,
      relocationAggregateId: '00112233-4455-6677-8899-aabbccddeeff',
      requestContentType: 'application/json',
      request,
      reservationToken: handoffId,
      reservedPayloadBytes: 1n
    },
    targetSpotId: 'room-target',
    targetNodeRid: Buffer.from('target'),
    targetNodeGeneration: 6n,
    targetSpotGeneration: 6n,
    targetAuthorityOwnerGeneration: 12n,
    operationId: { high: 7n, low: 8n },
    replyContentType: SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
    reply
  };
  const release = DefaultZLinkSpotManager.prototype.beginActorJoinRelocation.call(
    manager as never,
    handoffId
  );
  try {
    await DefaultZLinkSpotManager.prototype.restoreCanonicalActorJoinRecovery.call(
      manager as never,
      recovery
    );
    assert.equal(preparedContentType, 'application/json');
    t.mock.timers.tick(30_001);
    assert.notEqual(
      admissions.get(handoffId),
      undefined,
      'started restore owns admission lifetime'
    );
  } finally {
    release();
    assert.equal(admissions.get(handoffId), undefined, 'unsuccessful terminal releases admission');
  }
});

test('canonical ActorJoin admission expires while only prepared', (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const admissions = new ZLinkFormalRemoteActorAdmissionRegistry();
  const actorRef = {
    actorId: 'prepared-only',
    objectGeneration: 5n,
    nodeRid: 'source',
    meshName: 'mesh-a'
  };
  admissions.begin({
    actorId: actorRef.actorId,
    actorType: 'Player',
    actorRef,
    spotId: 'room-target',
    targetSpotGeneration: 6n,
    expectedMembershipEpoch: 1n,
    requestFingerprint: '',
    transferId: 'prepared-only'
  });
  admissions.complete('prepared-only', { accepted: true, actorRef });
  t.mock.timers.tick(30_001);
  assert.equal(admissions.get('prepared-only'), undefined);
});

test('canonical ActorJoin factory failure releases the claimed admission', async () => {
  const harness = createActorJoinHostHarness({ canonicalRecovery: true });
  harness.targetActorManager.prepareRelocationActor = async () => {
    assert.equal(harness.canonicalAdmissionState(), 'restoring');
    throw new Error('Canonical target factory failed.');
  };
  try {
    await assert.rejects(harness.relocate(), (error: unknown) => {
      assert.ok(error instanceof ZLinkFrameworkException);
      assert.equal(error.kind, ZLinkFrameworkErrorKind.InternalFailure);
      return true;
    });
    assert.equal(harness.canonicalAdmissionState(), undefined);
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
    const stateChunkCount = harness.controlKinds.filter((kind) => kind === 'state').length;
    assert.ok(
      stateChunkCount > 1,
      `a 16-byte advertised cap must split the payload into multiple chunks, got ${stateChunkCount}`
    );
    assert.equal(harness.location.commits, 1);
  } finally {
    await harness.dispose();
  }
});

test('the 1000ms cutover wait records a Warning only; the source Preserve fence settles an unverified target', async () => {
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
  harness.location.liveLeases.set('source-owner', 3n);
  harness.location.liveLeases.set('target-owner', 14n);
  let reportFallback!: () => void;
  const fallbackObserved = new Promise<void>((resolve) => {
    reportFallback = resolve;
  });
  const originalWarn = console.warn;
  console.warn = (...args: unknown[]) => {
    if (args[0] === '[zlink.runtime.relocation.cutover_timeout]') {
      harness.events.push('fallback:1000');
      reportFallback();
    }
  };
  try {
    const relocation = harness.relocate();
    relocation.catch(() => undefined);
    await fallbackObserved;
    assert.deepEqual(harness.controlKinds, ['prepare', 'state', 'data', 'cutover']);
    assert.equal(observedFallbacks.includes(1_000), true);
    assert.equal(harness.location.commits, 0, 'the cutover wait must never start the target CAS');
    assert.equal(harness.targetStageCount(), 1, 'the target keeps staging with dispatch closed');
    assert.equal(harness.targetActorManager.published, 0);

    // Restore deadline: the source settles with its Preserve fence and keeps
    // the authority; the target observes the fence and discards its staging.
    await harness.reachRestoreDeadline();
    await assert.rejects(relocation, /Preserve fence/);
    assert.equal(
      harness.events.includes('source:rolled-back'),
      true,
      'the source resumes its work'
    );
    assert.equal(harness.location.preserves, 1, 'one Preserve CAS fences the target StoreVersion');
    assert.equal(harness.location.aborts, 0, 'the source does not abort target staging');
    await waitUntil(() => harness.targetStageCount() === 0);
    assert.equal(harness.location.commits, 0);
    assert.equal(harness.targetActorManager.published, 0);
  } finally {
    console.warn = originalWarn;
    globalThis.setTimeout = originalSetTimeout;
    await harness.dispose();
  }
});

for (const observed of [
  'normalized-target',
  'second-commit',
  'steady',
  'restarted-target',
  'restarted-source',
  'different-object',
  'same-generation',
  'older-generation'
] as const) {
  test(`source settlement after a second committed move: ${observed}`, async () => {
    let entered!: () => void;
    const readEntered = new Promise<void>((resolve) => {
      entered = resolve;
    });
    let release!: () => void;
    const readGate = new Promise<void>((resolve) => {
      release = resolve;
    });
    const harness = createActorJoinHostHarness({
      beforeSourceSettlementRead: async () => {
        entered();
        await readGate;
      }
    });
    harness.location.liveLeases.set('source-owner', 3n);
    harness.location.liveLeases.set('target-owner', 14n);
    const relocation = harness.relocate();
    const result = relocation.then(
      (value) => ({ value, error: undefined }),
      (error) => ({ value: undefined, error })
    );
    try {
      await readEntered;
      await harness.joinTargetControl();
      const first = await harness.location.readAuthority();
      assert.equal(first.kind, 'snapshot');
      assert.equal(first.ownerId, 'target-owner');
      assert.equal(harness.targetActorManager.published, 1, 'B ran after A→B commit');
      assert.equal(harness.events.includes('source:rolled-back'), false);
      const [window] = harness.sourceCutoverWindows();
      assert.ok(Object.isFrozen(window));
      assert.equal(Reflect.set(window!, 'frameCount', 0), false);
      assert.ok(
        window !== undefined && window.frameCount !== undefined && window.frameCount > 0,
        'the source retains the boundary batch while its read is blocked'
      );
      const codec = new ServiceRelocationAuthorityPayloadCodec();
      assert.equal(codec.read(first.payload), undefined, 'B is normalized before the source read');
      // A restarted node keeps its OwnerId and takes a new OwnerLeaseGeneration;
      // only the (OwnerId, OwnerLeaseGeneration) pair names the same owner.
      const second =
        observed === 'restarted-target'
          ? { ownerId: 'target-owner', leaseGeneration: 15n, rid: 'target', generation: 23n }
          : observed === 'restarted-source'
            ? { ownerId: 'source-owner', leaseGeneration: 4n, rid: 'source', generation: 24n }
            : { ownerId: 'third-owner', leaseGeneration: 21n, rid: 'third', generation: 22n };
      const secondPublication = codec.publish(first.payload, {
        reference: 'second-relocation-root',
        checksumCrc32c: 0,
        aggregateId: '00000000-0000-0000-0000-000000000092',
        aggregateGeneration: 1n,
        inventoryDigest: '0'.repeat(64),
        targetOwnerId: second.ownerId,
        targetOwnerLeaseGeneration: second.leaseGeneration
      });
      if (observed !== 'normalized-target')
        await harness.location.prepareAggregate({
          participants: [
            { expectedStoreVersion: first.storeVersion, authorityPayload: secondPublication }
          ],
          targetOwner: { ownerId: second.ownerId, leaseGeneration: second.leaseGeneration },
          targetDescriptor: { meshName: 'mesh-a', rid: second.rid },
          targetDescriptorLifecycleGeneration: second.generation
        });
      if (observed !== 'normalized-target')
        assert.deepEqual(await harness.location.commitAggregate(), { kind: 'committed' });
      if (observed === 'steady') {
        const current = await harness.location.readAuthority();
        assert.equal(current.kind, 'snapshot');
        const codec = new ServiceRelocationAuthorityPayloadCodec();
        const publication = codec.read(current.payload);
        await harness.location.compareExchangeAuthority(undefined, current.storeVersion, {
          kind: 'put',
          generationTransition: 'preserve',
          payload:
            publication === undefined
              ? current.payload
              : codec.clear(current.payload, publication.reference)
        });
      }
      const third = await harness.location.readAuthority();
      assert.equal(third.kind, 'snapshot');
      assert.equal(
        third.ownerId,
        observed === 'normalized-target' ? 'target-owner' : second.ownerId
      );
      if (observed !== 'normalized-target')
        assert.equal(third.ownerLeaseGeneration, second.leaseGeneration);
      assert.equal(
        harness.location.commits,
        observed === 'normalized-target' ? 1 : 2,
        'commits precede the source read release'
      );
      const read = harness.location.readAuthority.bind(harness.location);
      harness.location.readAuthority = async () => {
        const current = await read();
        return observed === 'different-object'
          ? { ...current, objectGeneration: 6n }
          : observed === 'same-generation'
            ? { ...current, authorityOwnerGeneration: 11n }
            : observed === 'older-generation'
              ? { ...current, authorityOwnerGeneration: 10n }
              : current;
      };
      const preserves = harness.location.preserves;
      release();
      const outcome = await result;
      assert.equal(
        harness.events.includes('source:rolled-back'),
        false,
        'A must never resume beside a committed owner'
      );
      assert.equal(harness.location.preserves, preserves, 'A must not Preserve after B→C commit');
      assert.equal(
        window?.frameCount,
        undefined,
        'settlement releases the retained boundary batch'
      );
      if (
        observed === 'normalized-target' ||
        observed === 'second-commit' ||
        observed === 'steady' ||
        observed === 'restarted-target' ||
        observed === 'restarted-source'
      ) {
        assert.equal(outcome.error, undefined, 'A→B must settle as committed after B→C');
        assert.equal(harness.sourceCommits.length, 1);
        await harness.sourceLeaveIdle();
        const [route, ref, fence] = harness.sourceCommits[0] as any[];
        assert.equal(route.targetNodeRid, 'target');
        assert.equal(ref.nodeRid, 'target');
        if (observed === 'normalized-target') {
          assert.equal(fence.ownerId, first.ownerId);
          assert.equal(fence.authorityOwnerGeneration, first.authorityOwnerGeneration.toString());
          assert.equal(harness.sourceHandoff.messageFollowCount(), 1);
          await harness.lateMessage();
          assert.deepEqual(harness.followed, ['target']);
        } else {
          assert.equal(fence, null);
          assert.equal(harness.sourceHandoff.messageFollowCount(), 0);
          await assert.rejects(
            harness.lateSourceRequest(),
            (error: unknown) =>
              error instanceof ZLinkFrameworkException &&
              error.kind === ZLinkFrameworkErrorKind.Unavailable
          );
          await assert.rejects(
            harness.lateSourceRequest(false),
            (error: unknown) =>
              error instanceof ZLinkFrameworkException &&
              error.kind === ZLinkFrameworkErrorKind.NotFound
          );
          assert.deepEqual(harness.followed, []);
        }
        await harness.deliverSourceLeaveAgain();
        assert.equal(harness.events.filter((event) => event === 'source:removed').length, 1);
        assert.equal(harness.events.includes('source:discarded'), false);
      } else {
        assert.equal(outcome.error?.name, 'ServiceRelocationAuthorityError');
        assert.equal(harness.sourceCommits.length, 0);
        assert.equal(harness.events.filter((event) => event === 'source:discarded').length, 1);
      }
    } finally {
      release();
      await result;
      await harness.dispose();
    }
  });
}

test('target commit then Missing authority ends source through the expired-owner terminal', async () => {
  const harness = createActorJoinHostHarness({ missingAfterTargetCommit: true });
  harness.location.liveLeases.set('source-owner', 3n);
  harness.location.liveLeases.set('target-owner', 14n);
  harness.releaseAccepted();
  try {
    const handoff = await harness.relocate(undefined, false);
    await assert.rejects(
      handoff.sourceCleanup!(),
      (error: unknown) =>
        error instanceof Error &&
        error.name === 'ServiceRelocationAuthorityError' &&
        /Missing/.test(error.message)
    );
    assert.equal(harness.location.commits, 1);
    assert.equal((await harness.location.readAuthority()).kind, 'missing');
    assert.ok(harness.events.indexOf('target:closed') > harness.events.indexOf('dispatch:open'));
    assert.ok(harness.events.indexOf('source:discarded') > harness.events.indexOf('target:closed'));
    assert.equal(harness.events.filter((event) => event === 'source:discarded').length, 1);
    assert.equal(harness.events.includes('source:rolled-back'), false);
  } finally {
    await harness.dispose();
  }
});

test('an expired source owner lease at the Restore deadline ends the unit as an authority error', async () => {
  const harness = createActorJoinHostHarness({ dropCutover: true });
  harness.location.liveLeases.set('target-owner', 14n);
  try {
    const relocation = harness.relocate();
    relocation.catch(() => undefined);
    await waitUntil(() => harness.controlKinds.includes('cutover'));
    await harness.reachRestoreDeadline();
    await assert.rejects(
      relocation,
      (error: unknown) => error instanceof Error && error.name === 'ServiceRelocationAuthorityError'
    );
    assert.equal(harness.location.aborts, 0, 'an expired owner runs no Preserve fence');
    assert.equal(harness.events.includes('source:discarded'), true);
    assert.equal(harness.events.includes('source:rolled-back'), false);
    assert.equal(harness.location.commits, 0);
  } finally {
    await harness.dispose();
  }
});

test('ActorJoin READY submit failure retains staging without automatic PREPARE resend', async () => {
  const harness = createActorJoinHostHarness({ readyResults: [SubmitResult.NotConnected] });
  harness.location.liveLeases.set('source-owner', 3n);
  const relocation = harness.relocate();
  void relocation.catch(() => undefined);
  try {
    await harness.waitForReadyAttempt(1);
    harness.restoreSourceRoute();
    await turn();
    assert.equal(harness.prepareFingerprints.length, 1);
    assert.equal(harness.events.filter((value) => value === 'ready:submit').length, 1);
    assert.equal(harness.location.commits, 0);
    assert.equal(harness.targetStageCount(), 1);
    assert.equal(harness.location.aborts, 0);
    assert.equal(harness.targetActorManager.aborted, 0);
    assert(!harness.events.includes('source:rolled-back'));
    await harness.reachRestoreDeadline();
    await assert.rejects(
      relocation,
      (error) =>
        error instanceof ZLinkBackendResultError && error.result === RequestResult.NotConnected
    );
    assert.equal(harness.location.preserves, 1);
    assert(harness.events.includes('source:rolled-back'));
  } finally {
    await harness.dispose();
  }
});

test('exact ActorJoin Prepare via public ingress reuses staging after an undelivered READY', async () => {
  const harness = createActorJoinHostHarness({ readyResults: [SubmitResult.NotConnected] });
  harness.location.liveLeases.set('source-owner', 3n);
  const relocation = harness.relocate();
  void relocation.catch(() => undefined);
  try {
    await harness.waitForReadyAttempt(1);
    assert.equal(harness.targetStageCount(), 1);
    const restored = harness.events.filter((value) => value === 'restore:hidden').length;
    const duplicateReplies = await harness.deliverPrepareAgain();
    assert.equal(duplicateReplies.length, 1);
    assert.equal(decodeServiceRelocationControlResponse(duplicateReplies[0]!).kind, 'ready');
    assert.equal(harness.events.filter((value) => value === 'restore:hidden').length, restored);
    assert.equal(harness.prepareFingerprints.length, 1, 'sender never resubmits PREPARE');
    assert.equal(harness.targetStageCount(), 1);
    assert.equal(harness.targetActorManager.aborted, 0);
    assert.equal(harness.location.commits, 0);
    await harness.reachRestoreDeadline();
    await assert.rejects(
      relocation,
      (error) =>
        error instanceof ZLinkBackendResultError && error.result === RequestResult.NotConnected
    );
    assert.equal(harness.location.preserves, 1);
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
    await new Promise<void>((resolve) => setTimeout(resolve, 200));
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

test(
  'a target-side restore failure delivers Failed(53) end to end and restores source memory ' +
    'before any deadline',
  async () => {
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
      const started = performance.now();
      await assert.rejects(harness.relocate(), (error: unknown) => {
        assert.ok(
          error instanceof ZLinkFrameworkException,
          'must reject with a typed framework exception'
        );
        assert.equal(
          error.kind,
          ZLinkFrameworkErrorKind.InternalFailure,
          'a factory failure classifies as requestFailed(17) -> InternalFailure'
        );
        return true;
      });
      assert.ok(
        performance.now() - started < 5_000,
        'the explicit Failed must resolve the relocation well before the 30s control deadline'
      );
      assert.equal(
        harness.location.aborts,
        0,
        'a restore failure before reservation must not create an aggregate marker to abort'
      );
      assert.equal(
        harness.events.includes('source:rolled-back'),
        true,
        'the source in-memory seal must still roll back before the failure returns'
      );
    } finally {
      console.warn = originalWarn;
      console.error = originalError;
      await harness.dispose();
    }
  }
);

test('a relocation target Restore holds one activation admission from Prepare until target commit', async () => {
  // MeshNode §5.1 Pending activation: the target MeshNode counts the Restore from the moment it
  // receives the Prepare until the target commit, in the same record placement reads.
  let events: string[] = [];
  const activationAdmission = new ZLinkActivationAdmission(() => 1);
  const observeAdmission = (meshName: string) => {
    const current = activationAdmission.current(meshName);
    assert.equal(activationAdmission.hasHeadroom(meshName), current.active === 0);
    events.push(`activation:${meshName}:${current.active}/${current.limit}`);
  };
  const acquire = activationAdmission.acquire.bind(activationAdmission);
  activationAdmission.acquire = async (meshName, signal) => {
    const release = await acquire(meshName, signal);
    observeAdmission(meshName);
    return () => {
      release();
      observeAdmission(meshName);
    };
  };
  const harness = createActorJoinHostHarness({ holdAccepted: true, activationAdmission });
  events = harness.events;
  try {
    await harness.relocate();
    harness.releaseAccepted();
    await harness.targetIdle();
    assert.deepEqual(
      harness.events.filter(
        (value) => value.startsWith('activation:') || value === 'cas' || value === 'dispatch:open'
      ),
      ['activation:mesh-a:1/1', 'cas', 'activation:mesh-a:0/1', 'dispatch:open']
    );
    assert.deepEqual(activationAdmission.current('mesh-a'), { active: 0, limit: 1 });
  } finally {
    await harness.dispose();
  }
});

test('a relocation target Restore that fails releases its activation admission', async () => {
  const activationAdmission = new ZLinkActivationAdmission(() => 1);
  const harness = createActorJoinHostHarness({ activationAdmission });
  let heldDuringRestore: unknown;
  harness.targetActorManager.prepareRelocationActor = async () => {
    heldDuringRestore = activationAdmission.current('mesh-a');
    throw new Error('Target factory failed for this test.');
  };
  const originalWarn = console.warn;
  const originalError = console.error;
  console.warn = () => {};
  console.error = () => {};
  try {
    await assert.rejects(harness.relocate());
    assert.deepEqual(heldDuringRestore, { active: 1, limit: 1 });
    assert.deepEqual(activationAdmission.current('mesh-a'), { active: 0, limit: 1 });
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
    assert.deepEqual(
      harness.events.filter(
        (value) => value.startsWith('source:onLeave') || value === 'source:removed'
      ),
      ['source:onLeave:started', 'source:onLeave:completed', 'source:removed']
    );
  } finally {
    console.warn = originalWarn;
    await harness.dispose();
  }
});

test('post-commit route cleanup failure preserves Accepted and is diagnosed once', async (t) => {
  const failure = new Error('route cleanup failed after target commit');
  const diagnostics: unknown[][] = [];
  t.mock.method(console, 'error', (...args: unknown[]) => diagnostics.push(args));
  let attempts = 0;
  const harness = createActorJoinHostHarness({
    reconcileStatefulAuthorityRoutes: async () => {
      attempts++;
      throw failure;
    }
  });
  try {
    const result = await harness.relocate();
    assert.equal(String(result.actorRef.nodeRid), 'target');
    await harness.sourceLeaveIdle();
    await new Promise<void>((resolve) => setImmediate(resolve));
    assert.equal(diagnostics.length, 1);
    assert.strictEqual(diagnostics[0]![1], failure);
    assert.equal(harness.events.filter((event) => event === 'source:removed').length, 1);
    assert.equal(attempts, 1);
    const authority = await harness.location.readAuthority();
    assert.equal(authority.kind, 'snapshot');
    if (authority.kind === 'snapshot') assert.equal(authority.ownerId, 'target-owner');
  } finally {
    await harness.dispose();
  }
});

test('source retirement completes registry removal and reports all failed stages once', async (t) => {
  const leaveFailure = new Error('source leave failed');
  const retireFailure = new Error('source registry cleanup failed');
  const diagnostics: unknown[][] = [];
  t.mock.method(console, 'error', (...args: unknown[]) => diagnostics.push(args));
  const harness = createActorJoinHostHarness({
    sourceLeaveFailure: leaveFailure,
    sourceRetirementFailure: retireFailure
  });
  try {
    await harness.relocate();
    await harness.sourceLeaveIdle();
    // The retirement diagnostic follows the asynchronous manager completion.
    await new Promise<void>((resolve) => setImmediate(resolve));
    assert.equal(harness.events.filter((event) => event === 'source:removed').length, 1);
    assert.equal(diagnostics.length, 1);
    assert.ok(diagnostics[0]![1] instanceof AggregateError);
    assert.deepEqual((diagnostics[0]![1] as AggregateError).errors, [leaveFailure, retireFailure]);
    assert.equal(await harness.deliverSourceLeaveAgain(), true);
    harness.completeSourceCleanup();
    await Promise.resolve();
    assert.equal(harness.events.filter((event) => event === 'source:removed').length, 1);
    assert.equal(diagnostics.length, 1);
  } finally {
    await harness.dispose();
  }
});

test('post-commit source cleanup failure preserves Accepted and reports all failures once', async (t) => {
  const failure = new Error('source cleanup failed after target commit');
  const leaveFailure = new Error('source leave failed after target commit');
  const retirementFailure = new Error('source disposal failed after target commit');
  const diagnostics: unknown[][] = [];
  t.mock.method(console, 'error', (...args: unknown[]) => diagnostics.push(args));
  let attempts = 0;
  let routeAttempts = 0;
  const harness = createActorJoinHostHarness({
    sourceLeaveFailure: leaveFailure,
    sourceRetirementFailure: retirementFailure,
    commitSource: async () => {
      attempts++;
      throw failure;
    },
    reconcileStatefulAuthorityRoutes: async () => {
      routeAttempts++;
    }
  });
  try {
    const result = await harness.relocate();
    assert.equal(String(result.actorRef.nodeRid), 'target');
    assert.equal(attempts, 1);
    assert.equal(routeAttempts, 0);
    await harness.sourceLeaveIdle();
    await new Promise<void>((resolve) => setImmediate(resolve));
    assert.equal(harness.events.filter((event) => event === 'source:removed').length, 1);
    assert.equal(diagnostics.length, 1);
    assert.ok(diagnostics[0]![1] instanceof AggregateError);
    assert.deepEqual((diagnostics[0]![1] as AggregateError).errors, [
      failure,
      leaveFailure,
      retirementFailure
    ]);
  } finally {
    await harness.dispose();
  }
});

interface NativeRouteChangePeers {
  source: { raw: RawServiceMeshRuntime };
  target: { raw: RawServiceMeshRuntime };
  until(predicate: () => boolean, label: string): Promise<void>;
  hold(): void;
  held(): boolean;
  release(): void;
  switchRoute(forceReplacement?: boolean): Promise<void>;
  close(): Promise<void>;
}

interface ActorJoinHarnessOptions {
  readonly realSourceCapture?: boolean;
  readonly sourceQuiescenceChanged?: (safe: boolean) => void;
  readonly explicitFailureCode?: number;
  readonly submitFailure?: unknown;
  readonly rollbackFailure?: unknown;
  readonly sessionOwnerRouteReady?: () => boolean;
  readonly waitSessionOwnerRoute?: (signal?: AbortSignal) => Promise<void>;
  readonly requestPeerReconcile?: () => Promise<void>;
  readonly transport?: NativeRouteChangePeers;
  readonly beforeControlSubmit?: (kind: string) => void;
  readonly missingAfterTargetCommit?: boolean;
  readonly beforeSourceSettlementRead?: () => Promise<void>;
  readonly sourceLeaveFailure?: Error;
  readonly sourceRetirementFailure?: Error;
  readonly commitSource?: () => Promise<void>;
  readonly reconcileStatefulAuthorityRoutes?: () => Promise<void>;
  readonly holdAccepted?: boolean;
  readonly holdSourceLeave?: boolean;
  readonly readyResult?: number;
  readonly readyResults?: number[];
  readonly sourceLeaveResult?: number;
  readonly dropCutover?: boolean;
  readonly canonicalRecovery?: boolean;
  readonly activationAdmission?: ZLinkActivationAdmission;
  readonly metrics?: ZLinkRuntimeMetrics;
}

function createActorJoinHostHarness(options: ActorJoinHarnessOptions = {}) {
  let sourceRouteGeneration = 1n;
  let sourceRouteReady = true;
  const sourceRouteObservers = new Set<(rid: string) => void>();
  const events: string[] = [];
  const controlKinds: string[] = [];
  const prepareFingerprints: string[] = [];
  const readyReplies: Buffer[] = [];
  const relocationId = '00000000-0000-0000-0000-000000000091';
  let canonicalHandoffId: string | undefined;
  const actorId = 'actor-host-profile';
  const completionOperationId = { high: 61n, low: 67n };
  let canonicalAdmissionOperationId = completionOperationId;
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
      spotGeneration: 4n,
      spotKind: ZLinkSpotKind.User
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
    spotNodes: new Map([
      [
        'mesh-a',
        {
          actorFactoryRegistrations: {
            Player: { implementation: class Player {}, relocation: { kind: 'recreate' } }
          }
        }
      ]
    ])
  };
  const sourceState: any = options.realSourceCapture
    ? new ZLinkActorRuntimeState(actorId)
    : {
        actorId,
        actorType: 'Player',
        actor: sourceActor,
        meshName: 'mesh-a',
        spotId: options.canonicalRecovery === true ? 'entry-spot-estimate' : 'source-room',
        spotMembershipEpoch: 8n,
        nativeActorRef: { actorId, generation: 5n, nodeRid: 'source' },
        locationGeneration: 11n,
        ownerLeaseGeneration: 3n
      };
  if (options.realSourceCapture) {
    sourceState.rememberMeshName('mesh-a');
    sourceState.getOrStartCreation('Player', false, async () => ({
      status: 'created',
      actor: sourceActor
    }));
    sourceState.bindActor(sourceActor, sourceActor.context);
    sourceState.setNativeActorRef({ actorId, generation: 5n, nodeRid: 'source' });
    sourceState.setJoinedSpot('source-room', undefined, 8n, 4n);
    sourceState.setLocationGeneration(11n);
    sourceState.setOwnerLeaseGeneration(3n);
  }
  const packets = ['B1', 'B2', 'D1'].map((label, index) => actorJoinHandoffPacket(index, label));
  let releaseAccepted = () => {};
  const acceptedGate =
    options.holdAccepted === true
      ? new Promise<void>((resolve) => {
          releaseAccepted = resolve;
        })
      : Promise.resolve();
  let releaseSourceLeave = () => {};
  const sourceLeaveGate =
    options.holdSourceLeave === true
      ? new Promise<void>((resolve) => {
          releaseSourceLeave = resolve;
        })
      : Promise.resolve();
  let sourceLeaveDone!: () => void;
  const sourceLeaveIdle = new Promise<void>((resolve) => {
    sourceLeaveDone = resolve;
  });
  const sourceCleanupRefs: unknown[] = [];
  const sourceLeaveSpotIds: unknown[] = [];
  const sourceActorManager = {
    detachRelocationSource(state: typeof sourceState) {
      sourceCleanupRefs.push(state.nativeActorRef);
    },
    async completeCoreRelocationSource(_actorId: string, state: typeof sourceState) {
      assert.equal(state, sourceState);
      events.push('source:removed');
      sourceLeaveDone();
      if (options.sourceRetirementFailure !== undefined) throw options.sourceRetirementFailure;
    }
  };
  const followed: string[] = [];
  const sourceHandoff = new ZLinkActorHandoffCoordinator({
    routedTransport: {
      async sendToSpot(route: { targetNodeRid: string }) {
        followed.push(route.targetNodeRid);
      }
    } as never,
    requestSource: () => ({
      meshName: 'mesh-a',
      objectGeneration: 5n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 3n,
      nodeRid: 'source',
      nodeGeneration: 2n,
      authorityOwnerGeneration: 11n
    }),
    validateReplySource: () => true,
    isCurrentActorRef: () => false
  });
  const sourceCommits: unknown[][] = [];
  const sourceSerial = new ZLinkSpotSerialExecutor(
    new ZLinkSpotSerialTurnExecutor(true, 'source-room' as never),
    ZLinkUserSpotExecutionMode.PerActor,
    'source-room'
  );
  const pendingCaptures: Promise<unknown>[] = [];
  const fifoAdmissions: Promise<void>[] = [];
  const resumeObservations: Array<{ label: string; preserves: number; moving: boolean }> = [];
  const originalRequests: Array<{ id?: bigint; deadlineMs?: number; timeoutMs: number }> = [];
  const capture = (label: string, index: number) => {
    const packet = actorJoinHandoffPacket(index, label);
    const parts = [Message.from('header:' + label), Message.from(label)];
    try {
      const captured = sourceHandoff.capture(
        'remote',
        actorId,
        parts,
        false,
        undefined,
        attachActorMessageFollowContext(sourceActorRef as never, packet.messageFollowContext)
      );
      assert.notEqual(captured, undefined);
      pendingCaptures.push(Promise.resolve(captured));
      events.push('source:capture:' + label);
    } finally {
      parts.forEach((p) => p.close());
    }
  };
  const membershipTurn = options.realSourceCapture
    ? new ZLinkSpotSerialTurnExecutor(true, 'source-room' as never)
    : undefined;
  const sourceActivation =
    membershipTurn === undefined
      ? undefined
      : new ZLinkSpotActivation({
          meshName: 'mesh-a',
          spotId: 'source-room',
          domain: {
            kind: 'user',
            executionMode: ZLinkUserSpotExecutionMode.PerActor,
            relocationCoordinationMode: 'framework_managed'
          },
          spotType: class Room {},
          spot: {},
          serial: membershipTurn,
          timers: {},
          actorHandlers: {},
          handlers: {}
        } as never);
  sourceActivation?.commitActorJoin(sourceActor as never);
  const actualMembership =
    sourceActivation === undefined
      ? undefined
      : new ZLinkSpotActorMembership({
          resolveActivation: () => sourceActivation
        });
  const membershipFailures: unknown[] = [];
  const prepareFailures: unknown[] = [];
  const freshIngress = () => {
    const activation = sourceActivation;
    assert.notEqual(activation, undefined);
    return resolveActorIngress({ actorId, parts: [] } as never, false, {
      resolveActor: () => sourceActor as never,
      hasDepartedActor: (id) => activation!.hasDepartedActor(id)
    }).kind;
  };
  const membership = {
    async beginActorTransfer() {
      events.push('source:membership-begin');
      if (options.realSourceCapture)
        await actualMembership!.beginActorTransfer('source-room', actorId);
      assert.equal(sourceState.isMoving, true);
      capture('B1', 0);
      capture('B2', 1);
    },
    async cancelActorTransfer() {
      events.push('source:membership-cancel');
      if (options.realSourceCapture) {
        if (options.rollbackFailure !== undefined)
          membershipTurn!.setLifecycleAdmissionClosed(true);
        try {
          await actualMembership!.cancelActorTransfer('source-room', actorId);
        } catch (error) {
          membershipFailures.push(error);
          throw error;
        }
        events.push('source:membership-restored');
      } else if (options.rollbackFailure !== undefined) throw options.rollbackFailure;
    },
    async commitActorLeaveAfterTransfer() {
      events.push('source:committed');
    },
    async prepareActorLeaveForTransfer() {
      events.push('source:onLeave:completed');
    },
    admitSourceActorPacketPrefix(id: string, records: readonly any[]) {
      events.push('source:fifo-admit');
      const admission = sourceSerial.admitActorPacketPrefix(id, records, async (_serial, parts) => {
        const label = Buffer.from(parts[1]!.data()).toString();
        const observation = { label, preserves: location.preserves, moving: sourceState.isMoving };
        resumeObservations.push(observation);
        events.push('source:handler:' + label);
        return undefined;
      });
      fifoAdmissions.push(admission.terminal);
      return admission;
    }
  };
  const realSourceActorTransfer = new ZLinkActorTransferRuntime({
    actorHandoff: sourceHandoff,
    spotManager: () => membership,
    actorManager: () => sourceActorManager,
    authorityStore: () => location,
    currentOwner: () => ({ ownerId: 'source-owner', leaseGeneration: 3n }),
    primaryMeshNode: () => sourceNode,
    routeTransport: {},
    notifyEntrySpotActorLeft: async () => {},
    locationLifecycle: () => undefined,
    clearRemoteActorPacketTarget() {},
    reportPostCommitError: (error: unknown) => deliveryErrors.push(error)
  } as never);
  const sourceActorTransfer = options.realSourceCapture
    ? realSourceActorTransfer
    : {
        async prepareMaintenanceSession(
          _actor: unknown,
          _state: unknown,
          _signal: unknown,
          _retainSource: unknown,
          relocation: { high: bigint; low: bigint }
        ) {
          sourceHandoff.begin(actorId, 5n);
          return {
            target: {
              routerChannelId: 'session.route',
              targetNodeRid: 'session-owner',
              spotId: 'session-entry',
              sessionNodeRid: 'session-owner',
              sessionRid: 'session-1',
              bindingGeneration: 5n,
              serviceWireRelocation: {
                relocation,
                coordinator: {
                  ownerId: sourceAuthority.ownerId,
                  leaseGeneration: sourceAuthority.ownerLeaseGeneration,
                  nodeRid: 'source',
                  nodeGeneration: 2n,
                  expectedAuthorityStoreVersion: sourceAuthority.storeVersion.value
                },
                session: {
                  sessionOwnerNodeRid: 'session-owner',
                  sessionOwnerNodeGeneration: 7n,
                  sessionOwnerId: 'session-owner-id',
                  sessionOwnerLeaseGeneration: 8n,
                  sessionRid: 'session-1',
                  bindingGeneration: 5n
                }
              }
            },
            handoffBacklog: packets.slice(0, 2),
            takeRelocationRelay: () => packets.slice(2),
            setReplayResults() {},
            async commit(...args: unknown[]) {
              sourceCommits.push(args);
              sourceHandoff.complete(
                actorId,
                args[0] as never,
                args[1] as never,
                [],
                args[2] as never
              );
              events.push('source:committed');
              await options.commitSource?.();
            },
            async rollback() {
              sourceHandoff.cancel(actorId);
              events.push('source:rolled-back');
              if (options.rollbackFailure !== undefined) throw options.rollbackFailure;
            },
            discard() {
              events.push('source:discarded');
            }
          };
        },
        async completeRelocationSourceLeave(_actor: unknown, sourceSpotId: unknown) {
          sourceLeaveSpotIds.push(sourceSpotId);
          events.push('source:onLeave:started');
          await sourceLeaveGate;
          if (options.sourceLeaveFailure !== undefined) throw options.sourceLeaveFailure;
          events.push('source:onLeave:completed');
        }
      };

  let targetState: Record<string, any> | undefined;
  let targetNativeAuthority:
    | {
        actor: { actorId: string; generation: bigint; nodeRid: string };
        authorityOwnerGeneration: bigint;
        spotId: string;
        spotGeneration: bigint;
        membershipEpoch: bigint;
      }
    | undefined;
  let releaseTargetPublication!: () => void;
  const targetPublication = new Promise<void>((resolve) => {
    releaseTargetPublication = resolve;
  });
  const targetActor = { context: { actorId, meshName: 'mesh-a' } };
  const targetActorManager = {
    published: 0,
    aborted: 0,
    async prepareRelocationActor(
      restoredActorId: string,
      _actorType: string,
      objectGeneration: bigint,
      authorityOwnerGeneration: bigint,
      spotId: string | undefined,
      spotGeneration: bigint,
      membershipEpoch: bigint
    ) {
      assert.equal(
        spotId,
        undefined,
        'Entry relocation preserves logical membership at the native boundary'
      );
      events.push('restore:hidden');
      targetNativeAuthority = {
        actor: { actorId: restoredActorId, generation: objectGeneration, nodeRid: 'target' },
        authorityOwnerGeneration,
        spotId: spotId ?? String(targetDescriptor.rid),
        spotGeneration:
          spotId === undefined ? targetDescriptor.lifecycleGeneration : spotGeneration,
        membershipEpoch
      };
      targetState = {
        actorId,
        nativeActorRef: { actorId, generation: 5n, nodeRid: 'target' },
        clearJoinedSpot() {
          this.spotId = undefined;
        },
        setJoinedSpot(spotId: unknown) {
          this.spotId = spotId;
        },
        installBoundSessionBinding(value: unknown) {
          this.boundSession = value;
          return true;
        }
      };
      return targetActor;
    },
    getState() {
      return targetState;
    },
    adoptCreatedAuthority() {
      const stage = [...((targetRuntime as any).targetStages as Map<string, any>).values()][0];
      const hidden = [...stage.staging.hidden.values()][0];
      const labels = hidden.replayPackets.map((packet: any) =>
        Buffer.from(packet.payload, 'base64').toString()
      );
      events.push(`queue:merged:${labels.join(',')}`);
      events.push('route:closed');
    },
    publishRelocationActor() {
      this.published += 1;
      events.push('dispatch:open');
      releaseTargetPublication();
    },
    async abortRelocationActor() {
      this.aborted += 1;
      events.push('restore:aborted');
    }
  };
  const admissions = new ZLinkFormalRemoteActorAdmissionRegistry();
  let restoredCanonicalRecovery: CanonicalActorJoinRecovery | undefined;
  if (options.canonicalRecovery !== true) {
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
      deferredJoinCompletion: { reference: 'accepted-root' } as never
    });
  }
  assert.notEqual(
    `${completionOperationId.high.toString(16)}:${completionOperationId.low.toString(16)}`,
    relocationId
  );

  let sourceRuntime!: ZLinkHostServiceRelocationRuntime;
  let targetRuntime!: ZLinkHostServiceRelocationRuntime;
  let sourceSignal = new AbortController();
  let sourceDeadlineAtMs: number | undefined;
  const targetDeliveries: Promise<void>[] = [];
  const sourceDeliveries: Promise<void>[] = [];
  const deliveryErrors: unknown[] = [];
  const taskShutdown = new AbortController();
  const taskErrorSink = new ZLinkRuntimeTaskErrorSink();
  taskErrorSink.onRuntimeTaskException(({ error }) => deliveryErrors.push(error));
  const detachedTaskRunner = new ZLinkRuntimeTaskRunner(taskErrorSink, taskShutdown.signal);
  const deliver = (
    runtime: ZLinkHostServiceRelocationRuntime,
    sourceNodeRid: string,
    bytes: Uint8Array,
    deliveries: Promise<void>[]
  ) => {
    const part = Message.from(bytes);
    const delivery = Promise.resolve(
      runtime.tryHandleControl('mesh-a', {
        kind: ReceiveKind.NodeSend,
        sourceNodeRid,
        parts: [part]
      } as never)
    )
      .then(() => undefined)
      .catch((error) => {
        deliveryErrors.push(error);
        if (runtime === targetRuntime && options.readyResults === undefined) {
          sourceSignal.abort(error);
        }
      })
      .finally(() => part.close());
    deliveries.push(delivery);
  };
  const sourceNode = {
    status: () => ({ routingId: 'source', lifecycleGeneration: 2n }),
    peers: () => [{ routingId: 'target', lifecycleGeneration: 6n, state: 3 }],
    sendInfrastructureControl(_targetRid: string, bytes: Uint8Array) {
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
      if (options.transport !== undefined) {
        if (control !== undefined) options.beforeControlSubmit?.(control.kind);
        return options.transport.source.raw
          .sendService(_targetRid, [bytes])
          .then((sent) => (sent ? SubmitResult.Ok : SubmitResult.NotConnected));
      }
      queueMicrotask(() => deliver(targetRuntime, 'source', Buffer.from(bytes), targetDeliveries));
      return SubmitResult.Ok;
    },
    observePeerRoute: () =>
      options.transport?.source.raw.observePeerRoute('target') ?? {
        generation: sourceRouteGeneration,
        ready: sourceRouteReady
      },
    observePeerRouteChanged: (listener: (rid: string) => void) => {
      if (options.transport !== undefined)
        return options.transport.source.raw.observePeerRouteChanged(listener);
      sourceRouteObservers.add(listener);
      return () => {
        sourceRouteObservers.delete(listener);
      };
    },
    observePeerConnectionIntentRemoved: () => () => {},
    requestInfrastructureControlFrames(
      targetRid: string,
      frames: readonly Uint8Array[],
      requestOptions?: {
        readonly timeoutMs?: number;
        readonly signal?: AbortSignal;
        readonly onSubmitted?: (signal: AbortSignal) => Promise<void>;
        readonly requestOperation?: PendingOperation<unknown>;
      }
    ): Promise<readonly Uint8Array[]> {
      return requestDurableService(
        sourceNode,
        targetRid,
        frames,
        requestOptions!.requestOperation!,
        'Prepare',
        requestOptions?.signal,
        requestOptions?.onSubmitted
      );
    },
    requestService(
      _targetRid: string,
      frames: readonly Uint8Array[],
      timeoutMs: number,
      onAdmitted?: () => void,
      pending?: Pick<PendingOperation<unknown>, 'id' | 'deadlineMs'>,
      receiver?: undefined,
      onSubmitted?: () => void
    ): Promise<readonly Buffer[]> {
      if (options.submitFailure !== undefined) throw options.submitFailure;
      originalRequests.push({ id: pending?.id, deadlineMs: pending?.deadlineMs, timeoutMs });
      if (options.realSourceCapture) {
        capture('D1', 2);
      }
      const bytes = frames[0]!;
      const control = decodeServiceRelocationControlRequest(bytes);
      if (control !== undefined) {
        controlKinds.push(control.kind);
        if (control.kind === 'prepare') {
          prepareFingerprints.push(Buffer.from(bytes).toString('base64'));
        }
      }
      if (options.transport !== undefined)
        return options.transport.source.raw.requestService(
          _targetRid,
          frames,
          timeoutMs,
          onAdmitted,
          pending,
          receiver,
          onSubmitted
        );
      const reply = new Promise<readonly Buffer[]>((resolve, reject) => {
        const parts = frames.map((frame) => Message.from(frame));
        let settled = false;
        const finish = (action: () => void) => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          action();
        };
        const timer = setTimeout(
          () => finish(() => reject(new Error('in-memory infrastructure request timed out'))),
          timeoutMs
        );
        const delivery = Promise.resolve(
          targetRuntime.tryHandleControl('mesh-a', {
            kind: ReceiveKind.NodeRequest,
            sourceNodeRid: 'source',
            parts,
            reply: (reply: Uint8Array | readonly Uint8Array[]) => {
              events.push('ready:submit');
              const queuedResult = options.readyResults?.shift();
              const result = queuedResult ?? options.readyResult ?? SubmitResult.Ok;
              if (result === SubmitResult.Ok) {
                const replyParts = Array.isArray(reply) ? reply : [reply];
                readyReplies.push(Buffer.from(replyParts[0]!));
                finish(() =>
                  resolve(
                    options.explicitFailureCode === undefined
                      ? replyParts.map((part) => Buffer.from(part))
                      : [
                          encodeServiceRelocationControlResponse({
                            ...decodeServiceRelocationControlResponse(Buffer.from(replyParts[0]!)),
                            kind: 'failed',
                            failureCode: options.explicitFailureCode
                          } as ServiceMaintenanceRelocationFailed)
                        ]
                  )
                );
              }
              if (result !== SubmitResult.Ok) {
                sourceRouteReady = false;
                const failure = new ZLinkBackendResultError('request', RequestResult.NotConnected);
                prepareFailures.push(failure);
                finish(() => reject(failure));
              }
              return result;
            }
          } as never)
        )
          .then(() => undefined)
          .catch((error) => {
            deliveryErrors.push(error);
            if (options.readyResults === undefined) sourceSignal.abort(error);
            finish(() => reject(error));
          })
          .finally(() => parts.forEach((part) => part.close()));
        targetDeliveries.push(delivery);
      });
      onSubmitted?.();
      return reply;
    }
  };
  const targetNode = {
    isPeerRouteReady(peerRid: string, generation: bigint) {
      return (
        peerRid === 'session-owner' &&
        generation === 7n &&
        (options.sessionOwnerRouteReady?.() ?? true)
      );
    },
    waitForPeerRoute(
      _peerRid: string,
      _generation: bigint,
      _deadline: bigint,
      signal?: AbortSignal
    ) {
      assert.notEqual(options.waitSessionOwnerRoute, undefined);
      return options.waitSessionOwnerRoute!(signal);
    },
    status: () => ({ routingId: 'target', lifecycleGeneration: 6n }),
    peers: () => [{ routingId: 'source', lifecycleGeneration: 2n, state: 3 }],
    actorLookup(lookupActorId: string) {
      assert.equal(lookupActorId, actorId);
      assert.notEqual(targetNativeAuthority, undefined);
      return {
        actor: targetNativeAuthority!.actor,
        spotId: targetNativeAuthority!.spotId,
        spotGeneration: targetNativeAuthority!.spotGeneration,
        membershipEpoch: targetNativeAuthority!.membershipEpoch
      };
    },
    restoreActorAuthority(
      restoredActorId: string,
      stableType: string,
      objectGeneration: bigint,
      authorityOwnerGeneration: bigint,
      spotId: string,
      spotGeneration: bigint,
      membershipEpoch: bigint
    ) {
      assert.equal(stableType, 'Player');
      targetNativeAuthority = {
        actor: { actorId: restoredActorId, generation: objectGeneration, nodeRid: 'target' },
        authorityOwnerGeneration,
        spotId,
        spotGeneration,
        membershipEpoch
      };
      events.push(`raw-authority:${authorityOwnerGeneration}`);
      return targetNativeAuthority.actor;
    },
    sendInfrastructureControl(_sourceRid: string, bytes: Uint8Array) {
      events.push('sourceLeave:submit');
      if (options.sourceLeaveResult !== undefined) return options.sourceLeaveResult;
      if (options.transport !== undefined)
        return options.transport.target.raw
          .sendService(_sourceRid, [bytes])
          .then((sent) => (sent ? SubmitResult.Ok : SubmitResult.NotConnected));
      queueMicrotask(() => deliver(sourceRuntime, 'target', Buffer.from(bytes), sourceDeliveries));
      return SubmitResult.Ok;
    },
    sendToNode(_sourceRid: string, bytes: Uint8Array) {
      events.push('sourceLeave:submit');
      if (options.transport !== undefined)
        return options.transport.target.raw
          .sendService(_sourceRid, [bytes])
          .then((sent) => (sent ? SubmitResult.Ok : SubmitResult.NotConnected));
      const result = options.sourceLeaveResult ?? SubmitResult.Ok;
      if (result === SubmitResult.Ok) {
        queueMicrotask(() =>
          deliver(sourceRuntime, 'target', Buffer.from(bytes), sourceDeliveries)
        );
      }
      return result;
    }
  };
  const targetSpotManager = {
    admitSourceActorPacketPrefix(_actorId: string, records: readonly any[]) {
      const terminals = targetMailbox.admitDurablePrefix(
        records.map((record) => ({
          operation: (executeChild: any) =>
            record.drain((parts: readonly Message[]) =>
              executeChild(() => {
                events.push(`replay:${Buffer.from(parts[1]!.data()).toString()}`);
              })
            ),
          preparation: record.preparation,
          workOptions: { onWithdrawable: record.onWithdrawable }
        }))
      );
      return { terminal: Promise.all(terminals).then(() => undefined) };
    },
    beginActorJoinRelocation: DefaultZLinkSpotManager.prototype.beginActorJoinRelocation,
    formalRemoteActorAdmissions: admissions,
    formalRemoteTransfers: { delete() {} },
    activations: { resolve: () => undefined },
    resolveRelocationActivation: () => undefined,
    options: {
      entryNodeRid: 'entry-target',
      detachedTaskRunner,
      canonicalActorJoinResolver: async () => ({ actorType: 'Player' }),
      actorResolver: () => undefined,
      async dispatchEntryActorJoin() {
        events.push('onJoined');
      },
      actorTransferRuntime: {
        prepareDeferredJoinAccepted() {
          events.push('recovery:prepared');
          return { reference: 'recovered-root' };
        },
        async deliverDeferredJoinCompletion() {
          events.push('accepted:started');
          await acceptedGate;
          events.push('accepted:completed');
        }
      }
    },
    dispatchMeshActorJoin: DefaultZLinkSpotManager.prototype.dispatchMeshActorJoin,
    // The User Spot lifecycle-lane wrapper delegates to this body.
    dispatchMeshActorJoinCore: (
      DefaultZLinkSpotManager.prototype as unknown as { dispatchMeshActorJoinCore: unknown }
    ).dispatchMeshActorJoinCore,
    async restoreCanonicalActorJoinRecovery(
      recovery: CanonicalActorJoinRecovery,
      signal?: AbortSignal
    ) {
      restoredCanonicalRecovery = recovery;
      await DefaultZLinkSpotManager.prototype.restoreCanonicalActorJoinRecovery.call(
        this as never,
        recovery,
        signal
      );
    },
    finalizeActorJoinRelocation: DefaultZLinkSpotManager.prototype.finalizeActorJoinRelocation,
    openActorJoinRelocation: DefaultZLinkSpotManager.prototype.openActorJoinRelocation
  };
  let canonicalIngress:
    | ((record: {
        readonly command: number;
        readonly flags: number;
        readonly sourceRoutingId: string;
        readonly requestSequence: bigint;
        readonly parts: readonly Buffer[];
        readonly applicationJobOwner: ReturnType<typeof actorJoinApplicationJobOwner>;
      }) => Promise<unknown>)
    | undefined;
  let canonicalReply: ((parts: readonly Buffer[]) => void) | undefined;
  let canonicalMailboxTask = Promise.resolve();
  const targetCanonicalRuntime = new ServiceStatefulRuntime(
    {
      topology: {
        peer: (rid: string) =>
          rid === 'source'
            ? {
                descriptor: {
                  lifecycleGeneration: 2n,
                  protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY]
                }
              }
            : undefined
      },
      mailbox: {
        tryEnqueue(record: { readonly stateful?: ServiceStatefulMailboxData }) {
          const stateful = record.stateful!;
          canonicalMailboxTask = (async () => {
            const requestPayload = stateful.canonicalApplicationPayload!;
            const request = Message.from(requestPayload.payload);
            try {
              await DefaultZLinkSpotManager.prototype.dispatchMeshActorJoin.call(
                targetSpotManager as never,
                'mesh-a',
                { spotId: 'entry-target' } as never,
                {
                  kindData: stateful.kindData,
                  parts: [request],
                  contentType: requestPayload.contentType,
                  isPending: () => true,
                  replyActorJoin: (joinResult: 0 | 1) =>
                    stateful.reply?.(RequestResult.Ok, 0, undefined, {
                      kind: 'actorJoin',
                      joinResult,
                      spot: { spotId: 'entry-target', generation: 6n },
                      membershipEpoch: 8n
                    }) === true
                      ? SubmitResult.Ok
                      : SubmitResult.NotConnected
                } as never
              );
            } finally {
              request.close();
            }
          })();
          return true;
        }
      },
      observePeerRoute: () => ({ generation: 1n, ready: true }),
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved() {
        return () => {};
      },
      setServiceIngress(handler: typeof canonicalIngress) {
        canonicalIngress = handler;
      },
      replyService(_record: unknown, parts: readonly Buffer[]) {
        canonicalReply?.(parts.map((part) => Buffer.from(part)));
      }
    } as never,
    'target',
    6n
  );
  const canonicalTargetSpot = targetCanonicalRuntime.restoreUserSpotAuthority(
    'entry-target',
    'Entry',
    6n,
    1n
  );
  const sourceCanonicalRuntime = new ServiceStatefulRuntime(
    {
      topology: {
        peer: (rid: string) =>
          rid === 'target'
            ? {
                descriptor: {
                  lifecycleGeneration: 6n,
                  protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY]
                }
              }
            : undefined
      },
      observePeerRoute: () => ({ generation: 1n, ready: true }),
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved() {
        return () => {};
      },
      setServiceIngress() {},
      async requestService(_rid: string, parts: readonly Buffer[]) {
        const reply = new Promise<readonly Buffer[]>((resolve) => {
          canonicalReply = resolve;
        });
        const applicationJobOwner = actorJoinApplicationJobOwner();
        try {
          await canonicalIngress!({
            command: 28,
            flags: 0,
            sourceRoutingId: 'source',
            requestSequence: 1n,
            parts,
            applicationJobOwner
          });
          await canonicalMailboxTask;
          return await reply;
        } finally {
          applicationJobOwner.close();
        }
      }
    } as never,
    'source',
    2n
  );
  sourceCanonicalRuntime.restoreUserSpotAuthority('source-room', 'Room', 4n, 1n);
  const canonicalSourceActor = sourceCanonicalRuntime.restoreActorAuthority(
    actorId,
    'Player',
    5n,
    11n,
    'source-room',
    4n,
    7n
  );
  sourceCanonicalRuntime.rememberSpotRoute({
    spot: canonicalTargetSpot.ref,
    targetNodeRid: 'target',
    targetNodeGeneration: 6n,
    authorityOwnerGeneration: canonicalTargetSpot.authorityOwnerGeneration,
    ownerLeaseGeneration: 14n,
    storeVersion: 'target-entry-v1'
  });
  const common = {
    registration,
    locationStore: () => location,
    liveDescriptors: async () => [targetDescriptor],
    completions: () => undefined,
    spotNodeRuntime: () => undefined,
    metrics: options.metrics,
    boundSessionRelocation: undefined
  };
  sourceRuntime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    ...common,
    locationStore: () =>
      options.missingAfterTargetCommit
        ? {
            ...location,
            async compareExchangeAuthority() {
              assert.fail('Missing must not attempt source Preserve');
            },
            async readAuthority() {
              const current = await location.readAuthority();
              if (current.kind === 'snapshot' && current.ownerId === 'target-owner') {
                await targetPublication;
                await targetRuntime.dispose();
                events.push('target:closed');
                const closedAuthority = await location.readAuthority();
                assert.ok(closedAuthority.kind === 'snapshot');
                assert.equal(closedAuthority.ownerId, 'target-owner');
                const deleted = await location.compareExchangeAuthority(
                  encodeAuthorityKey('actor', actorId),
                  closedAuthority.storeVersion,
                  { kind: 'delete' }
                );
                assert.equal(deleted.kind, 'deleted');
                return location.readAuthority();
              }
              return current;
            }
          }
        : {
            ...location,
            async readAuthority() {
              if (controlKinds.includes('cutover')) await options.beforeSourceSettlementRead?.();
              return location.readAuthority();
            }
          },
    reconcileStatefulAuthorityRoutes: options.reconcileStatefulAuthorityRoutes,
    currentOwner: () => ({ ownerId: 'source-owner', leaseGeneration: 3n }),
    localDescriptor: () => ({ rid: 'source', lifecycleGeneration: 2n }),
    meshNode: () => sourceNode,
    spotManager: () => undefined,
    actorManager: () => sourceActorManager,
    actorTransfer: sourceActorTransfer,
    sourceQuiescenceChanged: () =>
      options.sourceQuiescenceChanged?.(sourceRuntime.isSafeToShutdown())
  } as never);
  const targetMailbox = new ZLinkActorSerialExecutor(actorId, 'entry-target');
  const targetTransfer = new ZLinkActorTransferRuntime({
    actorHandoff: new ZLinkActorHandoffCoordinator({
      routedTransport: {} as never,
      requestSource: () => ({
        meshName: 'mesh-a',
        objectGeneration: 5n,
        ownerId: 'target-owner',
        ownerLeaseGeneration: 14n,
        nodeRid: 'target',
        nodeGeneration: 6n,
        authorityOwnerGeneration: 12n
      }),
      validateReplySource: () => true
    }),
    spotManager: () => targetSpotManager,
    reportPostCommitError: (error: unknown) => deliveryErrors.push(error)
  } as never);
  targetRuntime = new ZLinkHostServiceRelocationRuntime({
    requests: fixtureRequestRuntime(),
    ...common,
    requestPeerReconcile: options.requestPeerReconcile,
    activationAdmission: options.activationAdmission,
    currentOwner: () => ({ ownerId: 'target-owner', leaseGeneration: 14n }),
    localDescriptor: () => targetDescriptor,
    meshNode: () => targetNode,
    spotManager: () => targetSpotManager,
    actorManager: () => targetActorManager,
    actorTransfer: {
      admitRelocationPrefix: targetTransfer.admitRelocationPrefix.bind(targetTransfer),
      async publishRoutedActorOwnership() {
        if (options.realSourceCapture) assert.equal(targetState?.boundSession, undefined);
        else assert.notEqual(targetState?.boundSession, undefined);
        events.push('command44');
      }
    }
  } as never);

  if (options.transport !== undefined) {
    for (const [peer, runtime] of [
      [options.transport.source, sourceRuntime],
      [options.transport.target, targetRuntime]
    ] as const) {
      peer.raw.setServiceIngress(async (record) => {
        const parts = record.parts.map((part) => Message.from(part));
        try {
          const handled = await runtime.tryHandleControl('mesh-a', {
            kind:
              record.requestSequence === undefined ? ReceiveKind.NodeSend : ReceiveKind.NodeRequest,
            sourceNodeRid: record.sourceRoutingId,
            parts,
            reply: (reply: Uint8Array | readonly Uint8Array[]) => {
              if (record.reply === undefined)
                throw new Error('Native request has no reply capability.');
              record.reply(Array.isArray(reply) ? reply : [reply as Uint8Array]);
              return SubmitResult.Ok;
            }
          } as never);
          return handled ? 'infrastructure' : undefined;
        } finally {
          for (const part of parts) part.close();
        }
      });
    }
  }
  const relocate = async (advertisedReceiveChunkLimitBytes?: number, settleSource = true) => {
    sourceSignal = new AbortController();
    if (options.canonicalRecovery === true && canonicalHandoffId === undefined) {
      const admission = sourceCanonicalRuntime.joinActorCanonical(
        canonicalSourceActor.ref,
        'target',
        canonicalTargetSpot.ref,
        canonicalTargetSpot.ref.generation,
        {
          packetName: '__zlink.actor.join_spot.request',
          contentType: 'application/json',
          payload: Buffer.from('canonical-request')
        },
        {
          targetNodeGeneration: 2n,
          authorityOwnerGeneration: 11n,
          ownerLeaseGeneration: 3n
        },
        { phase: 'admission', transferId: 'private-local-transfer' },
        5_000
      );
      const admissionResult = await admission.promise;
      canonicalAdmissionOperationId = { high: 2n, low: admission.id };
      assert.equal(admissionResult.terminalResult, RequestResult.Ok);
      assert.equal(admissionResult.kindData?.kind, 'actorJoinCompletion');
      canonicalHandoffId =
        admissionResult.kindData?.kind === 'actorJoinCompletion'
          ? admissionResult.kindData.canonicalHandoffId
          : undefined;
      assert.notEqual(canonicalHandoffId, undefined);
    }
    const activeRelocationId =
      options.canonicalRecovery === true
        ? `${canonicalHandoffId!.slice(0, 8)}-${canonicalHandoffId!.slice(8, 12)}` +
          `-${canonicalHandoffId!.slice(12, 16)}-${canonicalHandoffId!.slice(16, 20)}` +
          `-${canonicalHandoffId!.slice(20)}`
        : relocationId;
    sourceDeadlineAtMs = performance.now() + 30_000;
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
      relocationId: activeRelocationId,
      completionOperationId,
      ...(options.canonicalRecovery !== true
        ? {}
        : {
            canonicalRecovery: {
              handoffId: canonicalHandoffId!,
              admissionOperationId: canonicalAdmissionOperationId,
              requestContentType: 'application/json',
              request: Buffer.from('canonical-request'),
              replyContentType: 'application/octet-stream',
              reply: Buffer.alloc(0),
              actorNodeGeneration: 2n,
              expectedOwnerLeaseGeneration: 3n,
              targetNodeGeneration: 6n,
              targetSpotGeneration: 6n,
              targetAuthorityOwnerGeneration: 12n,
              targetSpotAuthorityOwnerGeneration: 1n
            }
          }),
      advertisedReceiveChunkLimitBytes,
      signal: sourceSignal.signal,
      deadlineAtMs: sourceDeadlineAtMs
    });
    if (settleSource) await result.sourceCleanup?.();
    // The public Join owner installs the target ref before the one-way source
    // cleanup terminal arrives. Source cleanup must retain its original ref.
    if (options.realSourceCapture) {
      sourceState.setNativeActorRef(result.actorRef);
      sourceState.setJoinedSpot('entry-target');
    } else {
      sourceState.nativeActorRef = result.actorRef;
      sourceState.spotId = 'entry-target';
    }
    return result;
  };
  return {
    async waitForReadyAttempt(count: number) {
      const deadline = performance.now() + 1000;
      while (
        events.filter((value) => value === 'ready:submit').length < count &&
        performance.now() < deadline
      )
        await turn();
      assert(events.filter((value) => value === 'ready:submit').length >= count);
    },
    restoreSourceRoute() {
      sourceRouteGeneration++;
      sourceRouteReady = true;
      for (const observer of sourceRouteObservers) observer('target');
    },
    stopSource: () => sourceRuntime.dispose(),
    sourceState,
    sourceActivation,
    freshIngress,
    membershipFailures,
    prepareFailures,
    resumeObservations,
    originalRequests,
    sourceFifoIdle: async () => {
      await Promise.all(pendingCaptures);
      await Promise.all(fifoAdmissions);
    },
    events,
    controlKinds,
    prepareFingerprints,
    readyReplies,
    location,
    targetActorManager,
    targetAuthorityGeneration: () => targetNativeAuthority?.authorityOwnerGeneration,
    canonicalRecoveryIdentity: () => restoredCanonicalRecovery,
    canonicalAdmissionState: () =>
      canonicalHandoffId === undefined ? undefined : admissions.get(canonicalHandoffId)?.state,
    canonicalAdmissionActorNodeRid: () => {
      if (canonicalHandoffId === undefined) return undefined;
      return String(admissions.get(canonicalHandoffId)?.admission.actorRef.nodeRid);
    },
    relocate,
    releaseAccepted,
    releaseSourceLeave,
    sourceLeaveIdle: async () => {
      await sourceLeaveIdle;
      await sourceRuntime.drainActorJoinSources();
    },
    sourceCleanupRefs,
    sourceCommits,
    sourceHandoff,
    followed,
    async lateSourceRequest(withFollowContext = true) {
      const parts = [
        Message.from(
          encodeStreamHeader({
            kind: ZLinkStreamMessageKind.Request,
            flags: ZLinkStreamHeaderFlags.HasRequestSeq,
            codec: ZLinkStreamCodec.Json,
            requestSeq: 1n,
            name: 'late',
            metadata: new Map()
          })
        ),
        Message.from('{}')
      ];
      const context = createInitialActorMessageFollowContext(
        {
          actorRef: sourceActorRef as never,
          meshName: 'mesh-a',
          ownerId: sourceAuthority.ownerId,
          ownerLeaseGeneration: sourceAuthority.ownerLeaseGeneration,
          ownerNodeGeneration: 2n,
          authorityOwnerGeneration: sourceAuthority.authorityOwnerGeneration
        } as never,
        parts,
        true
      );
      const dispatch = new ZLinkSpotActorPacketDispatch({
        spot: {} as never,
        spotId: () => 'source-room',
        registry: {} as never,
        resolveActor: () => {
          assert.ok(events.includes('source:removed'));
          return undefined;
        },
        async onDisconnectActor() {},
        routeBeforeLocal: (delivery) => {
          const captured = sourceHandoff.capture(
            delivery.origin,
            delivery.actorId,
            delivery.parts,
            delivery.returnResponse,
            delivery.remoteBoundSessionTarget,
            delivery.fallbackActorRef
          );
          return captured === undefined ? undefined : { handled: true, response: captured };
        }
      });
      try {
        return await dispatch.dispatch(
          undefined,
          'remote',
          actorId,
          parts,
          true,
          undefined,
          withFollowContext
            ? attachActorMessageFollowContext(sourceActorRef as never, context)
            : (sourceActorRef as never)
        );
      } finally {
        parts.forEach((part) => part.close());
      }
    },
    async lateMessage() {
      const packet = actorJoinHandoffPacket(3, 'late');
      const parts = [Message.from('header:late'), Message.from('late')];
      try {
        return await sourceHandoff.capture(
          'remote',
          actorId,
          parts,
          false,
          undefined,
          attachActorMessageFollowContext(sourceActorRef as never, packet.messageFollowContext)
        );
      } finally {
        parts.forEach((part) => part.close());
      }
    },
    joinTargetControl: () => Promise.all(targetDeliveries),
    sourceCutoverWindows: () => [sourceRuntime[inspectRuntime]().sourceCutoverWindow],
    sourceLeaveSpotIds,
    sourceProfileCount: () => sourceRuntime[inspectRuntime]().sourceActorJoinProfiles,
    completeSourceCleanup: () => sourceRuntime.completeActorJoinSourceCleanup(actorId),
    async targetIdle() {
      await waitUntil(() => targetRuntime[inspectRuntime]().targetStages === 0);
      await Promise.all(targetDeliveries);
      await targetMailbox.execute(() => undefined);
      assert.deepEqual(deliveryErrors, []);
    },
    async deliverPrepareAgain(
      leaseGeneration?: bigint,
      waitForReply = true,
      manifestConflict = false
    ) {
      const original = Buffer.from(prepareFingerprints[0]!, 'base64');
      const prepare = decodeServiceRelocationControlRequest(
        original
      ) as ServiceMaintenanceRelocationPrepare;
      const part = Message.from(
        manifestConflict
          ? encodeServiceRelocationControlRequest({
              ...prepare,
              payloadTotalLength: prepare.payloadTotalLength + 1n
            })
          : leaseGeneration === undefined
            ? original
            : encodeServiceRelocationControlRequest({
                ...prepare,
                coordinator: { ...prepare.coordinator, leaseGeneration }
              })
      );
      const replies: Buffer[] = [];
      try {
        await targetRuntime.tryHandleControl('mesh-a', {
          kind: ReceiveKind.NodeRequest,
          sourceNodeRid: 'source',
          parts: [part],
          reply: (reply: Uint8Array | readonly Uint8Array[]) => {
            const frames = Array.isArray(reply) ? reply : [reply];
            replies.push(Buffer.from(frames[0]!));
            return SubmitResult.Ok;
          }
        } as never);
        if (waitForReply) await waitUntil(() => replies.length === 1);
        return replies;
      } finally {
        part.close();
      }
    },
    async deliverCutoverAgain() {
      const prepare = controlKinds;
      assert.equal(prepare[0], 'prepare');
      assert.ok(targetRuntime[inspectRuntime]().terminalTargets > 0);
      const wire = {
        kind: 'cutover',
        relocation: { high: 0n, low: 145n },
        targetAttemptGeneration: 6n,
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
      const part = Message.from(
        Buffer.from(
          JSON.stringify({
            packetName: '__zlink.actor.source_leave.terminal',
            transferId: relocationId,
            actorId,
            succeeded: true
          })
        )
      );
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
    targetStageCount: () => targetRuntime[inspectRuntime]().targetStages,
    reachRestoreDeadline: async () => {
      assert.notEqual(sourceDeadlineAtMs, undefined);
      while (performance.now() < sourceDeadlineAtMs!) {
        await new Promise<void>((resolve) =>
          setTimeout(resolve, Math.max(1, Math.ceil(sourceDeadlineAtMs! - performance.now())))
        );
      }
      sourceSignal.abort(new Error('Restore deadline reached.'));
    },
    clearDeliveryErrors: () => {
      deliveryErrors.length = 0;
    },
    async dispose() {
      try {
        releaseTargetPublication();
        sourceSignal.abort(new Error('ActorJoin harness disposed.'));
        taskShutdown.abort();
        releaseAccepted();
        releaseSourceLeave();
        admissions.delete(relocationId);
        if (canonicalHandoffId !== undefined) admissions.delete(canonicalHandoffId);
        await Promise.all([...targetDeliveries, ...sourceDeliveries]);
        await targetMailbox.close();
        await sourceSerial.close();
        sourceCanonicalRuntime.close();
        targetCanonicalRuntime.close();
        await sourceRuntime.dispose();
        await targetRuntime.dispose();
      } finally {
        membershipTurn?.closeAdmission();
        await sourceActivation?.serialExecutor.close();
        await membershipTurn?.whenIdle();
      }
    }
  };
}

function actorJoinLocationStore(initial: ZLinkAuthoritySnapshot, events: string[]) {
  let current = initial;
  let removed = false;
  let prepared: any;
  let commits = 0;
  let aborts = 0;
  let preserves = 0;
  return {
    get commits() {
      return commits;
    },
    get aborts() {
      return aborts;
    },
    get preserves() {
      return preserves;
    },
    async readAuthority() {
      return removed ? { kind: 'missing' as const, storeNow: new Date() } : current;
    },
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
      if (participant.expectedStoreVersion.value !== current.storeVersion.value) {
        return { kind: 'stale' };
      }
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
    async compareExchangeAuthority(_key: unknown, expected: any, mutation: any) {
      if (removed || expected.value !== current.storeVersion.value)
        return { kind: 'conflict', current: await this.readAuthority() };
      if (mutation.kind === 'delete') {
        removed = true;
        return { kind: 'deleted', storeNow: new Date() };
      }
      if (mutation.generationTransition === 'preserve') preserves += 1;
      current = {
        ...current,
        storeVersion: { value: `normalized-v${commits + preserves + 1}` } as never,
        payload: Buffer.from(mutation.payload)
      };
      return { ...current, kind: 'stored' };
    },
    /** Owners whose lease reads as live; every other owner reads as missing. */
    liveLeases: new Map<string, bigint>(),
    async readOwnerLease(ownerId: string) {
      const storeNow = new Date();
      const leaseGeneration = this.liveLeases.get(ownerId);
      return leaseGeneration === undefined
        ? { kind: 'missing', storeNow }
        : {
            kind: 'found',
            token: { ownerId, leaseGeneration },
            leaseExpiresAt: new Date(storeNow.getTime() + 60_000),
            storeNow
          };
    }
  };
}

function actorJoinHandoffPacket(index: number, label: string) {
  const parts = [Message.from(`header:${label}`), Message.from(label)];
  try {
    const messageFollowContext = createInitialActorMessageFollowContext(
      {
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
      } as never,
      parts,
      false
    );
    return {
      index,
      header: Buffer.from(parts[0]!.data()).toString('base64'),
      payload: Buffer.from(parts[1]!.data()).toString('base64'),
      returnResponse: false,
      messageFollowContext
    };
  } finally {
    parts.forEach((part) => part.close());
  }
}

async function dispatchRelocationControl(
  runtime: ZLinkHostServiceRelocationRuntime,
  sourceNodeRid: string,
  control: ServiceMaintenanceRelocationControl
): Promise<void> {
  const part = Message.from(encodeServiceRelocationControlRequest(control));
  try {
    assert.equal(
      await runtime.tryHandleControl('mesh-a', {
        kind: ReceiveKind.NodeSend,
        sourceNodeRid,
        parts: [part]
      } as never),
      true
    );
  } finally {
    part.close();
  }
}

async function waitUntil(predicate: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    if (predicate()) return;
    await new Promise<void>((resolve) => setTimeout(resolve, 5));
  }
  assert.fail('condition did not become true');
}

test('a retired Session relocation cannot disconnect a successor Session route', async () => {
  const registry = new ZLinkActorSessionBindingRegistry();
  const events: string[] = [];
  const context = (routingId: string) => ({
    routingId,
    actorSlotControls: {
      enqueueBound: async (slot: number) => {
        events.push(`bound:${routingId}:${slot}`);
      },
      enqueueUnbound: async (slot: number) => {
        events.push(`unbound:${routingId}:${slot}`);
      }
    },
    bindLocal() {},
    unbindLocal() {}
  });
  const firstContext = context('session-old');
  const successorContext = context('session-new');
  const actor = (bindingGeneration: bigint) => ({
    actorId: 'actor-successor',
    ref: { actorId: 'actor-successor', objectGeneration: 1n, bindingGeneration }
  });
  await registry.bind(firstContext, actor(1n), 'old-token', undefined, 'session-old');
  const first = (await registry.route('actor-successor'))!;
  await registry.replace(first, successorContext, actor(2n), 'new-token', undefined, 'session-new');
  const transport = {
    send: async () => {
      throw new Error('unused');
    },
    disconnect: async (_actorId: string, options: { readonly bindingToken: string }) => {
      events.push(`disconnect:${options.bindingToken}`);
    }
  };
  const service = new ZLinkBoundSessionService(registry as never, undefined as never, {
    transport
  });

  await service.closeSessionBinding('actor-successor', {
    sessionRid: 'session-old',
    bindingGeneration: 1n
  });

  assert.equal((await registry.route('actor-successor'))?.bindingToken, 'new-token');
  assert.equal((await registry.route('actor-successor'))?.actorSlot, 1);
  assert.equal(events.includes('disconnect:new-token'), false);
  assert.equal(events.includes('unbound:session-new:1'), false);
});

test('an exact failed Session relocation closes its physical Session context', async () => {
  const registry = new ZLinkActorSessionBindingRegistry();
  let closed = 0;
  const context = {
    routingId: 'session-current',
    actorSlotControls: {
      enqueueBound: async () => {},
      enqueueUnbound: async () => {}
    },
    bindLocal() {},
    unbindLocal() {},
    close: async () => {
      closed += 1;
      await registry.cleanup(context);
    }
  };
  const actor = {
    actorId: 'actor-current',
    ref: { actorId: 'actor-current', objectGeneration: 1n, bindingGeneration: 2n }
  };
  await registry.bind(context, actor, 'current-token', undefined, 'session-current');
  const transport = {
    send: async () => {
      throw new Error('unused');
    },
    disconnect: async () => {}
  };
  const service = new ZLinkBoundSessionService(registry as never, undefined as never, {
    transport
  });

  await service.closeSessionBinding('actor-current', {
    sessionRid: 'session-current',
    bindingGeneration: 2n
  });

  assert.equal(closed, 1);
  assert.equal(await registry.route('actor-current'), undefined);
});

for (const phase of ['Prepare', 'cutover'] as const) {
  test(`native relocation retains its original terminal after ${phase} route replacement`, async () => {
    const { routeChangePeers } = require(
      process.cwd() + '/test/contract/helpers/route-change-peer'
    ) as {
      routeChangePeers(options: {
        sourceRid: string;
        targetRid: string;
        sourceGeneration: bigint;
        targetGeneration: bigint;
        runtimeRoot: string;
      }): Promise<NativeRouteChangePeers>;
    };
    const peers = await routeChangePeers({
      sourceRid: 'source',
      targetRid: 'target',
      sourceGeneration: 2n,
      targetGeneration: 6n,
      runtimeRoot: require('node:path').resolve(__dirname, '../../packages/framework/src/runtime')
    });
    let heldCutover = false;
    const harness = createActorJoinHostHarness({
      transport: peers,
      beforeControlSubmit: (kind) => {
        if (phase === 'cutover' && kind === 'cutover' && !heldCutover) {
          heldCutover = true;
          peers.hold();
        }
      }
    });
    harness.location.liveLeases.set('source-owner', 3n);
    try {
      await peers.until(
        () =>
          peers.source.raw.isPeerRouteReady('target') &&
          peers.target.raw.isPeerRouteReady('source'),
        'initial admission'
      );
      if (phase === 'Prepare') peers.hold();
      let outcome: Awaited<ReturnType<typeof harness.relocate>> | undefined;
      let failure: unknown;
      const relocation = harness.relocate().then(
        (value) => {
          outcome = value;
        },
        (error) => {
          failure = error;
        }
      );
      await peers.until(
        () => peers.held() && harness.prepareFingerprints.length > 0,
        `${phase} bytes retained on old pipe`
      );
      await peers.switchRoute(true);
      peers.release();
      if (phase === 'Prepare') await relocation;
      else
        await peers.until(
          () => outcome !== undefined || failure !== undefined,
          'original relocation terminal'
        );
      assert.equal(harness.originalRequests.length, 1);
      assert.equal(harness.prepareFingerprints.length, 1);
      if (phase === 'cutover')
        assert.equal(failure, undefined, 'CUTOVER whole-batch route replacement must commit');
      if (failure === undefined) {
        assert.equal(String(outcome!.actorRef.nodeRid), 'target');
        assert.equal(harness.location.commits, 1);
        assert.equal(harness.targetActorManager.published, 1);
        assert.equal(harness.targetActorManager.aborted, 0);
      } else {
        assert.equal(phase, 'Prepare');
        assert(failure instanceof ZLinkBackendResultError);
        assert.equal(failure.operation, 'request');
        assert.equal(failure.result, RequestResult.NotConnected);
        assert.equal(harness.location.commits, 0);
        assert.equal(harness.targetActorManager.published, 0);
        assert.equal(harness.location.preserves, 1);
        assert(harness.events.includes('source:rolled-back'));
      }
      assert.equal(new Set(harness.prepareFingerprints).size, 1);
      const dataCount = harness.controlKinds.filter((kind) => kind === 'data').length;
      const cutoverCount = harness.controlKinds.filter((kind) => kind === 'cutover').length;
      if (phase === 'cutover') {
        assert(
          cutoverCount >= 2,
          'a changed ready route replays the legitimate CUTOVER whole batch'
        );
        assert.equal(dataCount, cutoverCount);
      }
      console.log(
        'NATIVE_PHASE_EVIDENCE',
        JSON.stringify({
          phase,
          terminal: failure === undefined ? 'target' : 'failure',
          failureName: failure instanceof Error ? failure.name : undefined,
          failureMessage: failure instanceof Error ? failure.message : undefined,
          prepareCount: harness.prepareFingerprints.length,
          dataCount,
          cutoverCount,
          pendingId: String(harness.originalRequests[0]!.id),
          deadlineMs: harness.originalRequests[0]!.deadlineMs
        })
      );
      await relocation;
    } finally {
      await harness.dispose();
      await peers.close();
    }
  });
}

test('cross-node deferred Join waits for the Session owner route before committing', async () => {
  let routeReady = false;
  let release!: () => void;
  let requested!: () => void;
  const route = new Promise<void>((resolve) => {
    release = resolve;
  });
  const reconcile = new Promise<void>((resolve) => {
    requested = resolve;
  });
  const harness = createActorJoinHostHarness({
    canonicalRecovery: true,
    sessionOwnerRouteReady: () => routeReady,
    waitSessionOwnerRoute: () => route,
    requestPeerReconcile: async () => {
      requested();
    }
  });
  try {
    const relocation = harness.relocate();
    await reconcile;
    assert.equal(harness.location.commits, 0);
    assert.equal(harness.events.includes('ready:submit'), false);
    routeReady = true;
    release();
    assert.equal(String((await relocation).actorRef.nodeRid), 'target');
    await harness.targetIdle();
    assert.equal(harness.location.commits, 1);
    assert.equal(harness.events.includes('command44'), true);
  } finally {
    release();
    await harness.dispose();
  }
});

test('Session owner route deadline failure replies explicitly and restores the source', async () => {
  const { OperationTimeoutError } =
    await import('../../packages/framework/src/runtime/foundation/operation-registry');
  const harness = createActorJoinHostHarness({
    canonicalRecovery: true,
    sessionOwnerRouteReady: () => false,
    waitSessionOwnerRoute: async () => {
      throw new OperationTimeoutError(1n);
    },
    requestPeerReconcile: async () => {}
  });
  try {
    await assert.rejects(
      harness.relocate(),
      (error: unknown) =>
        error instanceof ZLinkFrameworkException &&
        error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
    );
    await harness.targetIdle();
    assert.equal(harness.location.commits, 0);
    assert.equal(harness.events.includes('source:rolled-back'), true);
    assert.equal(harness.controlKinds.includes('cutover'), false);
  } finally {
    await harness.dispose();
  }
});

test('processed PREPARE holds real source FIFO until Store Preserve response confirms storage', async () => {
  const harness = createActorJoinHostHarness({
    realSourceCapture: true,
    readyResults: [SubmitResult.NotConnected]
  });
  harness.location.liveLeases.set('source-owner', 3n);
  let entered!: () => void, release!: () => void;
  const preserving = new Promise<void>((resolve) => {
    entered = resolve;
  });
  const held = new Promise<void>((resolve) => {
    release = resolve;
  });
  const compare = harness.location.compareExchangeAuthority.bind(harness.location);
  harness.location.compareExchangeAuthority = async (key, expected, mutation) => {
    assert.equal(mutation.generationTransition, 'preserve');
    entered();
    await held;
    const stored = await compare(key, expected, mutation);
    harness.events.push('store:preserve-stored');
    return stored;
  };
  const relocation = harness.relocate();
  void relocation.catch(() => undefined);
  try {
    await preserving; // The original 30s absolute deadline is unchanged.
    assert.equal(harness.location.preserves, 0);
    assert.equal(harness.sourceState.isMoving, true);
    assert.equal(harness.resumeObservations.length, 0);
    assert.equal(harness.originalRequests.length, 1);
    assert.equal(harness.prepareFingerprints.length, 1);
    assert.equal(harness.events.filter((value) => value === 'restore:hidden').length, 1);
    assert.equal(harness.location.commits, 0);
    assert.equal(harness.targetActorManager.published, 0);
    release();
    await assert.rejects(
      relocation,
      (error) =>
        error instanceof ZLinkBackendResultError && error.result === RequestResult.NotConnected
    );
    await harness.sourceFifoIdle();
    assert.equal(harness.location.preserves, 1);
    assert.equal(harness.sourceState.isMoving, false);
    assert.deepEqual(
      harness.resumeObservations.map((value) => value.label),
      ['B1', 'B2', 'D1']
    );
    assert(harness.resumeObservations.every((value) => value.preserves === 1 && !value.moving));
    assert(
      harness.events.indexOf('store:preserve-stored') < harness.events.indexOf('source:fifo-admit')
    );
  } finally {
    release();
    await harness.dispose();
  }
});

test('uncertain Preserve response retains real source FIFO until Store readback and a confirmed fence', async () => {
  const harness = createActorJoinHostHarness({
    realSourceCapture: true,
    readyResults: [SubmitResult.NotConnected]
  });
  harness.location.liveLeases.set('source-owner', 3n);
  const compare = harness.location.compareExchangeAuthority.bind(harness.location);
  const read = harness.location.readAuthority.bind(harness.location);
  let uncertain = true,
    readbacks = 0;
  harness.location.readAuthority = async () => {
    if (!uncertain) readbacks++;
    return read();
  };
  harness.location.compareExchangeAuthority = async (key, expected, mutation) => {
    const result = await compare(key, expected, mutation);
    if (uncertain) {
      uncertain = false;
      assert.equal(harness.resumeObservations.length, 0);
      assert.equal(harness.sourceState.isMoving, true);
      throw new Error('Store applied Preserve but its response was lost');
    }
    assert(readbacks > 0);
    return result;
  };
  const relocation = harness.relocate();
  void relocation.catch(() => undefined);
  try {
    await harness.waitForReadyAttempt(1);
    await harness.reachRestoreDeadline();
    await assert.rejects(
      relocation,
      (error) =>
        error instanceof ZLinkBackendResultError && error.result === RequestResult.NotConnected
    );
    await harness.sourceFifoIdle();
    assert(readbacks > 0);
    assert.equal(harness.location.preserves, 2);
    assert.deepEqual(
      harness.resumeObservations.map((value) => value.label),
      ['B1', 'B2', 'D1']
    );
    assert(harness.resumeObservations.every((value) => value.preserves === 2 && !value.moving));
    assert.equal(harness.prepareFingerprints.length, 1);
  } finally {
    await harness.dispose();
  }
});

for (const proof of [
  'authority-missing',
  'lease-missing',
  'lease-expired',
  'replaced-lease',
  'foreign'
] as const) {
  test(`started PREPARE failure with ${proof} source proof discards and never resumes`, async () => {
    const harness = createActorJoinHostHarness({ readyResults: [SubmitResult.NotConnected] });
    harness.location.liveLeases.set('source-owner', 3n);
    const originalRead = harness.location.readAuthority.bind(harness.location);
    if (proof === 'replaced-lease') harness.location.liveLeases.set('source-owner', 4n);
    if (proof === 'lease-missing') harness.location.liveLeases.delete('source-owner');
    if (proof === 'lease-expired')
      harness.location.readOwnerLease = async () => ({
        kind: 'found',
        token: { ownerId: 'source-owner', leaseGeneration: 3n },
        leaseExpiresAt: new Date(42),
        storeNow: new Date(42)
      });
    const relocation = harness.relocate();
    void relocation.catch(() => undefined);
    try {
      await harness.waitForReadyAttempt(1);
      if (proof === 'authority-missing')
        harness.location.readAuthority = async () =>
          ({ kind: 'missing', storeNow: new Date() }) as never;
      if (proof === 'foreign')
        harness.location.readAuthority = async () =>
          ({
            ...(await originalRead()),
            ownerId: 'foreign-owner',
            ownerLeaseGeneration: 9n
          }) as never;
      await harness.reachRestoreDeadline();
      await assert.rejects(
        relocation,
        (error) => error instanceof Error && error.name === 'ServiceRelocationAuthorityError'
      );
      assert(harness.events.includes('source:discarded'));
      assert(!harness.events.includes('source:rolled-back'));
      assert.equal(harness.location.preserves, 0);
      assert.equal(harness.prepareFingerprints.length, 1);
    } finally {
      await harness.dispose();
    }
  });
}

for (const mode of ['explicit-failed', 'never-started', 'preserve', 'target'] as const) {
  for (const cleanupFails of [false, true]) {
    test(`source relocation metrics and original error/${mode}/cleanup=${cleanupFails}`, async (t) => {
      const collector = createRuntimeMetricCollector();
      const diagnostics: unknown[][] = [];
      t.mock.method(console, 'error', (...args: unknown[]) => diagnostics.push(args));
      const original = new Error('synchronous creation refusal');
      const cleanup = new Error('source cleanup failed');
      const windowCleanup: unknown[] = [];
      let endFollow!: () => void;
      const followEnded = new Promise<void>((resolve) => {
        endFollow = resolve;
      });
      const harness = createActorJoinHostHarness({
        metrics: new ZLinkRuntimeMetrics(collector.provider as never),
        ...(mode === 'explicit-failed' ? { explicitFailureCode: 35 } : {}),
        ...(mode === 'never-started' ? { submitFailure: original } : {}),
        ...(mode === 'preserve'
          ? {
              realSourceCapture: true,
              ...(cleanupFails
                ? { dropCutover: true }
                : { readyResults: [SubmitResult.NotConnected] })
            }
          : {}),
        ...(cleanupFails
          ? mode === 'target'
            ? {
                reconcileStatefulAuthorityRoutes: async () => {
                  throw cleanup;
                }
              }
            : mode === 'preserve'
              ? {
                  beforeSourceSettlementRead: async () => followEnded,
                  sourceQuiescenceChanged: (safe: boolean) => {
                    if (!safe) return;
                    harness.events.push('source:window-cleanup');
                    windowCleanup.push({
                      preserves: harness.location.preserves,
                      departed: harness.sourceActivation!.hasDepartedActor('actor-host-profile'),
                      ingress: harness.freshIngress(),
                      moving: harness.sourceState.isMoving,
                      fifoAdmitted: harness.events.includes('source:fifo-admit'),
                      outcome: collector.records.find(
                        (record) => record.name === 'zlink.relocation.completed'
                      )?.attributes?.outcome
                    });
                    throw cleanup;
                  }
                }
              : { rollbackFailure: cleanup }
          : {})
      });
      harness.location.liveLeases.set('source-owner', 3n);
      const resumeAtMetric: Array<{
        preserves: number;
        moving: boolean;
        fifoAdmitted: boolean;
      }> = [];
      const recordMetric = collector.records.push.bind(collector.records);
      assert.equal(Object.getOwnPropertyDescriptor(collector.records, 'push'), undefined);
      t.after(() => {
        Reflect.deleteProperty(collector.records, 'push');
      });
      collector.records.push = (...records: Parameters<typeof recordMetric>) => {
        for (const record of records) {
          if (record.name === 'zlink.relocation.route_convergence') endFollow();
          if (mode === 'preserve' && record.name === 'zlink.relocation.completed') {
            resumeAtMetric.push({
              preserves: harness.location.preserves,
              moving: harness.sourceState.isMoving,
              fifoAdmitted: harness.events.includes('source:fifo-admit')
            });
            harness.events.push('metric:source-terminal');
          }
        }
        return recordMetric(...records);
      };
      const keepAlive = mode === 'preserve' && cleanupFails ? new MessageChannel() : undefined;
      keepAlive?.port1.on('message', () => {});
      try {
        const relocation = harness.relocate();
        void relocation.catch(() => undefined);
        if (mode === 'preserve') {
          await harness.waitForReadyAttempt(1);
          await harness.reachRestoreDeadline();
        }
        if (mode === 'target') {
          const result = await relocation;
          assert.equal(String(result.actorRef.nodeRid), 'target');
          await harness.sourceLeaveIdle();
          await turn();
          assert.equal(diagnostics.length, cleanupFails ? 1 : 0);
          if (cleanupFails) assert.equal(diagnostics[0]![1], cleanup);
        } else
          await assert.rejects(relocation, (error) => {
            if (mode === 'never-started') assert.equal(error, original);
            if (mode === 'explicit-failed') {
              assert(error instanceof ZLinkFrameworkException);
              assert.equal(error.kind, ZLinkFrameworkErrorKind.DataLost);
            }
            if (mode === 'preserve')
              assert.equal(error, cleanupFails ? cleanup : harness.prepareFailures[0]);
            return true;
          });
        if (mode === 'preserve') {
          await harness.sourceFifoIdle();
          assert.equal(harness.sourceActivation!.hasDepartedActor('actor-host-profile'), false);
          assert.equal(harness.freshIngress(), 'local');
          assert.equal(harness.prepareFailures.length, cleanupFails ? 0 : 1);
          assert(
            harness.events.indexOf('source:membership-restored') <
              harness.events.indexOf('source:fifo-admit')
          );
          if (cleanupFails) {
            assert(
              harness.events.indexOf('metric:source-terminal') <
                harness.events.indexOf('source:window-cleanup')
            );
            assert.equal(
              collector.records.filter(
                (record) => record.name === 'zlink.relocation.route_convergence'
              ).length,
              1
            );
          }
          assert.deepEqual(
            windowCleanup,
            cleanupFails
              ? [
                  {
                    preserves: 1,
                    departed: false,
                    ingress: 'local',
                    moving: false,
                    fifoAdmitted: true,
                    outcome: 'aborted'
                  }
                ]
              : []
          );
          assert.deepEqual(resumeAtMetric, [{ preserves: 1, moving: false, fifoAdmitted: true }]);
          assert.deepEqual(
            harness.resumeObservations.map((value) => value.label),
            ['B1', 'B2', 'D1']
          );
          assert(
            harness.resumeObservations.every((value) => value.preserves === 1 && !value.moving)
          );
          assert(
            harness.events.indexOf('source:fifo-admit') <
              harness.events.indexOf('metric:source-terminal')
          );
          for (const name of [
            'zlink.relocation.started',
            'zlink.relocation.bytes',
            'zlink.relocation.duration'
          ])
            assert.equal(collector.records.filter((record) => record.name === name).length, 1);
        }
        const completed = collector.records.filter(
          (record) => record.name === 'zlink.relocation.completed'
        );
        assert.equal(completed.length, 1);
        assert.equal(
          completed[0]!.attributes!.outcome,
          mode === 'target' ? 'completed' : mode === 'preserve' ? 'aborted' : 'failed'
        );
        assert.equal(harness.location.preserves, mode === 'preserve' || mode === 'target' ? 1 : 0);
      } finally {
        keepAlive?.port1.close();
        keepAlive?.port2.close();
        await harness.dispose();
      }
    });
  }
}

test('source Preserve keeps membership rejection failed and preserves original request error', async (t) => {
  const collector = createRuntimeMetricCollector();
  t.mock.method(console, 'error', () => {});
  const harness = createActorJoinHostHarness({
    realSourceCapture: true,
    readyResults: [SubmitResult.NotConnected],
    rollbackFailure: new Error('close lifecycle admission'),
    metrics: new ZLinkRuntimeMetrics(collector.provider as never)
  });
  harness.location.liveLeases.set('source-owner', 3n);
  const relocation = harness.relocate();
  void relocation.catch(() => undefined);
  try {
    await harness.waitForReadyAttempt(1);
    await harness.reachRestoreDeadline();
    await assert.rejects(relocation, (error) => error === harness.prepareFailures[0]);
    await harness.sourceFifoIdle();
    assert.equal(harness.location.preserves, 1);
    assert.equal(harness.membershipFailures.length, 1);
    assert.match(String(harness.membershipFailures[0]), /committed Close/);
    assert.equal(harness.sourceActivation!.hasDepartedActor('actor-host-profile'), true);
    assert.equal(harness.freshIngress(), 'stale');
    assert.equal(harness.sourceState.isMoving, false);
    assert.deepEqual(
      harness.resumeObservations.map((value) => value.label),
      ['B1', 'B2', 'D1']
    );
    const completed = collector.records.filter(
      (record) => record.name === 'zlink.relocation.completed'
    );
    assert.equal(completed.length, 1);
    assert.equal(completed[0]!.attributes!.outcome, 'failed');
    for (const name of [
      'zlink.relocation.started',
      'zlink.relocation.bytes',
      'zlink.relocation.duration'
    ])
      assert.equal(collector.records.filter((record) => record.name === name).length, 1);
  } finally {
    await harness.dispose();
  }
});

for (const terminal of ['Store validation error', 'source disposal'] as const) {
  test(`uncertain PREPARE settlement ${terminal} never reopens real source FIFO`, async () => {
    const harness = createActorJoinHostHarness({
      realSourceCapture: true,
      readyResults: [SubmitResult.NotConnected]
    });
    harness.location.liveLeases.set('source-owner', 3n);
    const failure = new TypeError('invalid Store authority read');
    const relocation = harness.relocate();
    void relocation.catch(() => undefined);
    try {
      await harness.waitForReadyAttempt(1);
      if (terminal === 'Store validation error')
        harness.location.readAuthority = async () => {
          throw failure;
        };
      else await harness.stopSource();
      await assert.rejects(relocation, (error) =>
        terminal === 'Store validation error'
          ? error === failure
          : error instanceof Error &&
            error.name === 'AbortError' &&
            error.message === 'Relocation runtime stopped.'
      );
      assert.equal(harness.sourceState.isMoving, true);
      assert.equal(harness.resumeObservations.length, 0);
      assert(!harness.events.includes('source:fifo-admit'));
      assert.equal(harness.location.preserves, 0);
      assert.equal(harness.prepareFingerprints.length, 1);
    } finally {
      await harness.dispose();
    }
  });
}
