import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createHash } from 'node:crypto';
import { Message, SubmitResult } from '@zlink-systems/zlink';
import type {
  ZLinkMeshNodeDescriptor
} from '../../packages/framework/src/contracts';
import type {
  ZLinkAuthoritySnapshot
} from '../../packages/framework/src/contracts/Locations';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkObjectRole,
  ZLinkSpotKind,
  ZLinkTimerOverrunPolicy,
  ZLinkUserSpotExecutionMode
} from '../../packages/framework/src/contracts';
import { ZLinkHostServiceRelocationRuntime } from '../../packages/framework/src/runtime/host/service-relocation-host-runtime';
import {
  type ZLinkServiceRelocationControlRequest,
  type ZLinkServiceRelocationControlResponse
} from '../../packages/framework/src/runtime/host/service-relocation-control';
import {
  crc32c,
  encodeServiceRelocationEnvelope,
  inventoryDigest,
  ServiceDurableRelocationRuntime,
  ServiceRelocationAuthorityPayloadCodec,
  type ServiceRelocationEnvelope
} from '../../packages/framework/src/runtime/foundation/service-relocation-runtime';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import {
  ZLinkInMemoryAuthorityStore
} from '../../packages/framework/src/runtime/locations/in-memory-authority-store';
import {
  actorMessageFollowPayloadChecksum,
  messageFollowOwnerFenceKey,
  ownerFence
} from '../../packages/framework/src/runtime/actors/actor-message-follow-context';
import {
  decodeMaintenanceReplyRelayAck,
  encodeMaintenanceReplyRelay,
  encodeMaintenanceReplyRelayAck,
  encodeServiceWireFrozenActorApplicationRecord,
  type ServiceMaintenanceReplyRelay,
  type ServiceWireRequestSourceFence,
  type ServiceMaintenanceRelocationPrepare,
  type ServiceWireRelocationCandidate,
  type ServiceWireRelocationCoordinatorFence,
  type ServiceWireRelocationObject,
  type ServiceWireRelocationParticipant
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';

const spotKey = encodeAuthorityKey('user_spot', 'spot-a').value;
const actorKey = encodeAuthorityKey('actor', 'actor-a').value;
const acceptedOperationId = '11111111111111111111111111111111';
const acceptedOperation = {
  high: 0x1111111111111111n,
  low: 0x1111111111111111n
};

test('two host owners exchange canonical relocation reservation publish replay and seal commands', async () => {
  const events: string[] = [];
  const commands: number[] = [];
  const envelope = relocationEnvelope();
  const encodedEnvelope = encodeServiceRelocationEnvelope(envelope);
  const root = { reference: 'shared-root-a', checksumCrc32c: crc32c(encodedEnvelope) };
  const zeroEnvelope: ServiceRelocationEnvelope = {
    ...envelope,
    aggregateGeneration: 2n,
    participants: envelope.participants.map(participant => ({
      ...participant,
      queuedMessages: []
    }))
  };
  const encodedZeroEnvelope = encodeServiceRelocationEnvelope(zeroEnvelope);
  const zeroRoot = { reference: 'shared-root-zero', checksumCrc32c: crc32c(encodedZeroEnvelope) };
  const abortEnvelope: ServiceRelocationEnvelope = { ...envelope, aggregateGeneration: 3n };
  const encodedAbortEnvelope = encodeServiceRelocationEnvelope(abortEnvelope);
  const abortRoot = { reference: 'shared-root-abort', checksumCrc32c: crc32c(encodedAbortEnvelope) };
  const publication = {
    phase: 'sourceCleanupPending' as const,
    reference: root.reference,
    checksumCrc32c: root.checksumCrc32c,
    aggregateId: envelope.aggregateId,
    aggregateGeneration: envelope.aggregateGeneration,
    inventoryDigest: inventoryDigest(envelope.participants, envelope.memberships),
    targetOwnerId: 'owner-target',
    targetOwnerLeaseGeneration: 8n
  };
  const authorityPayload = new ServiceRelocationAuthorityPayloadCodec().publish(
    Buffer.from('authority-state'),
    publication
  );
  const authorities = new Map<string, ZLinkAuthoritySnapshot>([
    [spotKey, relocationAuthority('user_spot', 'SpotType')],
    [actorKey, relocationAuthority('actor', 'ActorType')]
  ]);
  let backpressureFirstControl = true;
  let source!: ZLinkHostServiceRelocationRuntime;
  const deliver = async (
    runtime: ZLinkHostServiceRelocationRuntime,
    sourceNodeRid: string,
    payload: Uint8Array
  ): Promise<void> => {
    const part = Message.from(payload);
    try {
      assert.equal(await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid,
        parts: [part]
      } as never), true);
    } finally {
      part.close();
    }
  };
  const target = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map([['mesh-a', {
      spotFactoryRegistrations: {
        SpotType: { implementation: class {}, relocation: { kind: 'recreate' } }
      },
      actorFactoryRegistrations: {
        ActorType: { implementation: class {}, relocation: { kind: 'recreate' } }
      }
    }]]) },
    locationStore: () => ({
      readAuthority: async (key: { readonly value: string }) => authorities.get(key.value)!,
      reserveRelocationCapacity: async (request: { readonly reservationId: string }) => {
        events.push('reserve-target-capacity');
        return {
          kind: 'reserved' as const,
          fence: { value: request.reservationId }
        };
      },
      abortRelocationCapacity: async () => {
        events.push('abort-target-capacity');
        return 'aborted' as const;
      },
      prepareAggregate: async (request: { readonly aggregateId: { readonly value: string };
        readonly aggregateGeneration: bigint }) => {
        events.push('reserve-capacity');
        return { kind: 'prepared' as const, fence: {
          aggregateId: request.aggregateId,
          aggregateGeneration: request.aggregateGeneration
        } };
      },
      commitAggregate: async () => {
        events.push('commit-capacity');
        for (const [key, current] of authorities) {
          authorities.set(key, {
            ...current,
            storeVersion: { value: `committed-${key}` } as never,
            authorityOwnerGeneration: 2n,
            ownerId: 'owner-target',
            ownerLeaseGeneration: 8n,
            payload: key === spotKey
              ? authorityPayload
              : new ServiceRelocationAuthorityPayloadCodec().publish(
                  current.payload,
                  publication
                )
          });
        }
        return { kind: 'committed' as const };
      },
      abortAggregate: async () => {
        events.push('abort-capacity');
        return { kind: 'aborted' as const };
      }
    }),
    relocationStore: () => ({
      read: async (reference: { readonly value: string }) =>
        reference.value === root.reference
          ? foundBlob(encodedEnvelope)
          : reference.value === zeroRoot.reference
            ? foundBlob(encodedZeroEnvelope)
          : reference.value === abortRoot.reference
            ? foundBlob(encodedAbortEnvelope)
          : { kind: 'missing' as const, storeNow: new Date() }
    }),
    currentOwner: () => ({ ownerId: 'owner-target', leaseGeneration: 8n }),
    liveDescriptors: async () => [],
    meshNode: () => ({
      status: () => ({ routingId: 'node-target', lifecycleGeneration: 12n }),
      sendToNode: (nodeRid: string, payload: Uint8Array) => {
        assert.equal(nodeRid, 'node-source');
        void deliver(source, 'node-target', payload);
        return SubmitResult.Ok;
      }
    }),
    completions: () => undefined,
    spotManager: () => ({
      prepareRelocationSpot: async () => {
        events.push('prepare-spot');
        return { spotId: 'spot-a', spot: {}, timers: { restoreRelocation: () => undefined },
          commitActorJoin: () => events.push('membership') };
      },
      publishRelocationSpot: async () => events.push('publish-spot'),
      abortRelocationSpot: async () => events.push('abort-spot'),
      dispatchRoutedActorPacket: async () => undefined
    }),
    actorManager: () => ({
      prepareRelocationActor: async () => {
        events.push('prepare-actor');
        return { context: { actorId: 'actor-a' }, configure() {} };
      },
      getState: () => ({
        spotId: 'spot-a',
        setJoinedSpot: () => events.push('joined'),
        setLocationGeneration: () => events.push('actor-authority'),
        setOwnerLeaseGeneration: () => undefined
      }),
      publishRelocationActor: () => events.push('publish-actor'),
      abortRelocationActor: () => events.push('abort-actor')
    }),
    actorTransfer: {} as never
  } as never);
  source = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map() },
    locationStore: () => undefined,
    relocationStore: () => undefined,
    currentOwner: () => ({ ownerId: 'owner-source', leaseGeneration: 7n }),
    liveDescriptors: async () => [],
    meshNode: () => ({
      status: () => ({ routingId: 'node-source', lifecycleGeneration: 11n }),
      sendToNode: (nodeRid: string, payload: Uint8Array) => {
        assert.equal(nodeRid, 'node-target');
        commands.push(payload[3]!);
        if (backpressureFirstControl) {
          backpressureFirstControl = false;
          return SubmitResult.Backpressured;
        }
        void deliver(target, 'node-source', payload);
        return SubmitResult.Ok;
      }
    }),
    completions: () => undefined,
    spotManager: () => undefined,
    actorManager: () => undefined,
    actorTransfer: {} as never
  } as never);

  const roundTrip = async (
    request: ZLinkServiceRelocationControlRequest
  ): Promise<ZLinkServiceRelocationControlResponse> => {
    return await (source as unknown as {
      sendControl(
        meshName: string,
        targetNodeRid: string,
        value: ZLinkServiceRelocationControlRequest
      ): Promise<ZLinkServiceRelocationControlResponse>;
    }).sendControl('mesh-a', 'node-target', request);
  };

  const prepare = relocationPrepare(envelope, root);
  const offered = await roundTrip(prepare);
  assert.equal(offered.kind, 'ready');
  if (offered.kind !== 'ready') return;
  assert.deepEqual(events, []);
  assert.deepEqual(await roundTrip(prepare), offered);
  assert.deepEqual(events, []);
  const zeroPrepare = relocationPrepare(zeroEnvelope, zeroRoot);
  const parallelOffer = await roundTrip(zeroPrepare);
  assert.equal(parallelOffer.kind, 'ready');
  if (parallelOffer.kind !== 'ready') return;
  assert.equal(parallelOffer.offeredMessages, 64n);
  assert.equal(parallelOffer.offeredBytes, 256n * 1024n * 1024n);
  const reserved = await roundTrip({ ...offered, role: 'source',
    offeredMessages: 0n, offeredBytes: 0n, participants: prepare.participants });
  assert.equal(reserved.kind, 'reserved');
  assert.deepEqual(events, ['reserve-target-capacity']);
  const prepared = await roundTrip({ kind: 'data', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', participantId: 1n, sequence: 1n, source: sourceFence(),
    object: prepare.object, phase: 'prepared' });
  assert.equal(prepared.kind, 'ack');
  assert.deepEqual(events, [
    'reserve-target-capacity', 'reserve-capacity',
    'prepare-spot', 'prepare-actor', 'joined', 'membership'
  ]);
  const committed = await roundTrip({ kind: 'data', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', participantId: 1n, sequence: 1n, source: sourceFence(),
    object: prepare.object, phase: 'committed' });
  assert.equal(committed.kind, 'ack');
  assert.equal(events.at(-1), 'commit-capacity');
  const ack = await roundTrip({ kind: 'data', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', participantId: 1n, sequence: 1n,
    frozenRecord: acceptedFrozenRecord(envelope, prepare.coordinator, prepare.candidate) });
  assert.equal(ack.kind, 'ack');
  if (ack.kind !== 'ack') return;
  const highWater = [
    { participantId: ack.participantId, highWater: ack.highWater },
    { participantId: 2n, highWater: 0n }
  ];
  const sealed = await roundTrip({ kind: 'seal', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', response: true, participants: highWater });
  assert.equal(sealed.kind, 'seal');
  const published = await roundTrip({ kind: 'complete', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', source: sourceFence(), sourceCleanupState: 'pending' });
  assert.equal(published.kind, 'complete');
  assert.deepEqual(commands, [40, 40, 40, 40, 30, 31, 31, 31, 34, 35]);
  assert.deepEqual(events, [
    'reserve-target-capacity', 'reserve-capacity',
    'prepare-spot', 'prepare-actor', 'joined', 'membership',
    'commit-capacity',
    'publish-spot', 'actor-authority', 'publish-actor'
  ]);

  const zeroOffer = await roundTrip(zeroPrepare);
  assert.equal(zeroOffer.kind, 'ready');
  if (zeroOffer.kind !== 'ready') return;
  assert.equal(zeroOffer.offeredMessages, 64n);
  assert.equal(zeroOffer.offeredBytes, 256n * 1024n * 1024n);
  assert.deepEqual(zeroOffer.participants, []);

  const targetInternals = target as unknown as {
    targetOffers: Map<string, unknown>;
    expireTargetOffer(stagingId: string, offer: unknown): Promise<void>;
  };
  const [zeroStagingId, zeroStoredOffer] = [...targetInternals.targetOffers.entries()][0]!;
  await targetInternals.expireTargetOffer(zeroStagingId, zeroStoredOffer);
  assert.equal(targetInternals.targetOffers.size, 0);

  const abortPublication = {
    ...publication,
    reference: abortRoot.reference,
    checksumCrc32c: abortRoot.checksumCrc32c,
    aggregateGeneration: abortEnvelope.aggregateGeneration
  };
  authorities.set(spotKey, relocationAuthority(
    'user_spot',
    'SpotType',
    new ServiceRelocationAuthorityPayloadCodec().publish(
      Buffer.from('authority-state'),
      abortPublication
    )
  ));
  authorities.set(actorKey, relocationAuthority('actor', 'ActorType'));
  const abortPrepare = relocationPrepare(abortEnvelope, abortRoot);
  const abortOffer = await roundTrip(abortPrepare);
  assert.equal(abortOffer.kind, 'ready');
  if (abortOffer.kind !== 'ready') return;
  await roundTrip({ ...abortOffer, role: 'source', offeredMessages: 0n,
    offeredBytes: 0n, participants: abortPrepare.participants });
  await roundTrip({ kind: 'data', relocation: abortPrepare.relocation,
    targetAttemptGeneration: abortPrepare.targetAttemptGeneration,
    coordinator: abortPrepare.coordinator, senderRole: 'source', participantId: 1n,
    sequence: 1n, source: sourceFence(), object: abortPrepare.object, phase: 'prepared' });
  const abortStart = events.length;
  const abortControl = { kind: 'data' as const, relocation: abortPrepare.relocation,
    targetAttemptGeneration: abortPrepare.targetAttemptGeneration,
    coordinator: abortPrepare.coordinator, senderRole: 'source' as const, participantId: 1n,
    sequence: 1n, source: sourceFence(), object: abortPrepare.object, phase: 'aborted' as const };
  const aborted = await roundTrip(abortControl);
  assert.equal(aborted.kind, 'ack');
  assert.deepEqual(events.slice(abortStart), ['abort-actor', 'abort-spot', 'abort-capacity']);
  await roundTrip(abortControl);
  assert.deepEqual(events.slice(abortStart), ['abort-actor', 'abort-spot', 'abort-capacity']);
});

test('production source and target runtimes complete standalone Actor relocation end to end', async () => {
  const events: string[] = [];
  const live = new Set([
    'mesh-a:node-source:11:owner-source:7',
    'mesh-a:node-target:12:owner-target:8'
  ]);
  const location = new ZLinkInMemoryAuthorityStore({
    isTargetLive(descriptor, lifecycle, owner) {
      return live.has(
        `${descriptor.meshName}:${descriptor.rid}:${lifecycle}:${owner.ownerId}:${owner.leaseGeneration}`
      );
    }
  }, () => new Date(100));
  const sourcePlacement = {
    meshName: 'mesh-a',
    nodeRid: 'node-source',
    nodeLifecycleGeneration: 11n,
    owner: { ownerId: 'owner-source', leaseGeneration: 7n }
  };
  const reserved = await location.reserve({
    key: { kind: 'actor', globalId: 'actor-production' },
    intent: {
      stableType: 'ActorType',
      requestContentReference: 'create:actor-production',
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 1n
    },
    target: sourcePlacement,
    creatingPayload: Buffer.from('creating'),
    capacity: { actors: 1, spots: 0 }
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  const creationTerminal = Buffer.from('actor-production-created');
  const activated = await location.completeCreation({
    key: { kind: 'actor', globalId: 'actor-production' },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: sourcePlacement,
    completion: {
      kind: 'created',
      readyPayload: Buffer.from('ready'),
      terminal: {
        operation: {
          sourceNodeRid: 'node-source',
          sourceNodeGeneration: 11n,
          operationId: { high: 0n, low: 1n }
        },
        terminalEnvelope: creationTerminal,
        terminalEnvelopeSha256: createHash('sha256').update(creationTerminal).digest(),
        operationDeadline: new Date(10_000)
      }
    }
  });
  assert.equal(activated.kind, 'created');
  if (activated.kind !== 'created') return;

  const relocation = new MemoryPublicRelocationStore();
  const targetLocation = new Proxy(location, {
    get(target, property, receiver) {
      if (property === 'reserveRelocationCapacity') {
        return async (...args: readonly unknown[]) => {
          events.push('target-capacity-reserved');
          return await (target.reserveRelocationCapacity as (...values: readonly unknown[]) =>
            Promise<unknown>).call(target, ...args);
        };
      }
      const value = Reflect.get(target, property, receiver);
      return typeof value === 'function' ? value.bind(target) : value;
    }
  });
  let source!: ZLinkHostServiceRelocationRuntime;
  let target!: ZLinkHostServiceRelocationRuntime;
  const deliver = async (
    runtime: ZLinkHostServiceRelocationRuntime,
    sourceNodeRid: string,
    payload: Uint8Array
  ): Promise<void> => {
    const part = Message.from(payload);
    try {
      assert.equal(await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid,
        parts: [part]
      } as never), true);
    } finally {
      part.close();
    }
  };
  const targetActor = { context: { actorId: 'actor-production' }, configure() {} };
  const targetState = {
    spotId: undefined as string | undefined,
    setJoinedSpot: (spotId: string) => {
      targetState.spotId = spotId;
      events.push('membership-restored');
    },
    setLocationGeneration: () => events.push('generation-published'),
    setOwnerLeaseGeneration: () => undefined,
    setRemoteBoundSessionTarget: () => undefined,
    setBoundSessionTransferTarget: () => undefined,
    setBoundSessionBindingGeneration: () => undefined
  };
  target = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map([['mesh-a', {
      actorFactoryRegistrations: {
        ActorType: { implementation: class {}, relocation: { kind: 'recreate' } }
      }
    }]]) },
    locationStore: () => targetLocation as never,
    relocationStore: () => relocation as never,
    currentOwner: () => ({ ownerId: 'owner-target', leaseGeneration: 8n }),
    liveDescriptors: async () => [],
    localDescriptor: () => ({ applicationVersion: 7n } as never),
    meshNode: () => ({
      status: () => ({ routingId: 'node-target', lifecycleGeneration: 12n }),
      sendToNode: (_nodeRid: string, payload: Uint8Array) => {
        void deliver(source, 'node-target', payload);
        return SubmitResult.Ok;
      }
    }),
    completions: () => undefined,
    spotManager: () => ({
      resolveRelocationActivation: () => undefined,
      dispatchRoutedActorPacket: async () => undefined
    }),
    actorManager: () => ({
      prepareRelocationActor: async () => {
        events.push('target-prepared');
        return targetActor;
      },
      getState: () => targetState,
      publishRelocationActor: () => events.push('target-published'),
      abortRelocationActor: async () => undefined
    }),
    actorTransfer: {
      publishRoutedActorOwnership: async () => events.push('route-published'),
      openRoutedActorSession: async () => events.push('target-open')
    }
  } as never);
  const sourceActor = { context: { actorId: 'actor-production' } };
  source = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map([['mesh-a', {
      actorFactoryRegistrations: {
        ActorType: { implementation: class {}, relocation: { kind: 'recreate' } }
      }
    }]]) },
    locationStore: () => location as never,
    relocationStore: () => relocation as never,
    currentOwner: () => ({ ownerId: 'owner-source', leaseGeneration: 7n }),
    liveDescriptors: async () => [],
    meshNode: () => ({
      status: () => ({ routingId: 'node-source', lifecycleGeneration: 11n }),
      sendToNode: (_nodeRid: string, payload: Uint8Array) => {
        void deliver(target, 'node-source', payload);
        return SubmitResult.Ok;
      }
    }),
    completions: () => undefined,
    spotManager: () => undefined,
    actorManager: () => ({
      completeRelocationSource: async () => events.push('source-released')
    }),
    actorTransfer: {
      prepareMaintenanceSession: async () => {
        events.push('source-sealed');
        return {
        target: undefined,
        handoffBacklog: [],
        setReplayResults: () => undefined,
        commit: async () => events.push('source-committed'),
        rollback: async () => events.push('source-rollback')
        };
      }
    }
  } as never);
  const sourceState = {
    actorId: 'actor-production',
    actor: sourceActor,
    actorType: 'ActorType',
    meshName: 'mesh-a',
    spotMembershipEpoch: 1n
  };
  await (source as unknown as {
    relocateStandaloneActor(
      meshName: string,
      state: unknown,
      target: ZLinkMeshNodeDescriptor,
      targetApplicationVersion: bigint
    ): Promise<void>;
  }).relocateStandaloneActor('mesh-a', sourceState, {
    rid: 'node-target',
    lifecycleGeneration: 12n,
    ownerId: 'owner-target',
    leaseGeneration: 8n,
    applicationVersion: 7n,
    entrySpotId: 'entry-target'
  } as never, 7n);

  const current = await location.readAuthority(
    encodeAuthorityKey('actor', 'actor-production')
  );
  assert.equal(current.kind, 'snapshot');
  if (current.kind !== 'snapshot') return;
  assert.equal(current.ownerId, 'owner-target');
  assert.equal(current.ownerLeaseGeneration, 8n);
  assert.ok(events.indexOf('target-capacity-reserved') < events.indexOf('source-sealed'));
  assert.ok(events.indexOf('target-prepared') < events.indexOf('target-published'));
  assert.ok(events.indexOf('target-published') < events.indexOf('route-published'));
  assert.ok(events.indexOf('route-published') < events.indexOf('target-open'));
  assert.ok(events.includes('source-committed'));
});

test('production restart recovery resumes a committed Actor authority and releases its durable root', async () => {
  const events: string[] = [];
  const location = new ZLinkInMemoryAuthorityStore({
    isTargetLive() {
      return true;
    }
  }, () => new Date(100));
  const sourcePlacement = {
    meshName: 'mesh-a',
    nodeRid: 'node-source',
    nodeLifecycleGeneration: 11n,
    owner: { ownerId: 'owner-source', leaseGeneration: 7n }
  };
  const reserved = await location.reserve({
    key: { kind: 'actor', globalId: 'actor-recovery' },
    intent: {
      stableType: 'ActorType',
      requestContentReference: 'create:actor-recovery',
      requestSha256: Buffer.alloc(32, 2),
      requestEncodedSize: 1n
    },
    target: sourcePlacement,
    creatingPayload: Buffer.from('creating'),
    capacity: { actors: 1, spots: 0 }
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  const terminal = Buffer.from('actor-recovery-created');
  const activated = await location.completeCreation({
    key: { kind: 'actor', globalId: 'actor-recovery' },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: sourcePlacement,
    completion: {
      kind: 'created',
      readyPayload: Buffer.from('ready'),
      terminal: {
        operation: {
          sourceNodeRid: 'node-source',
          sourceNodeGeneration: 11n,
          operationId: { high: 0n, low: 2n }
        },
        terminalEnvelope: terminal,
        terminalEnvelopeSha256: createHash('sha256').update(terminal).digest(),
        operationDeadline: new Date(10_000)
      }
    }
  });
  assert.equal(activated.kind, 'created');
  if (activated.kind !== 'created') return;

  const relocation = new MemoryPublicRelocationStore();
  const codec = new ServiceRelocationAuthorityPayloadCodec();
  const durable = new ServiceDurableRelocationRuntime(
    location as never,
    relocation.asRuntimePort(),
    codec
  );
  const actorAuthorityKey = encodeAuthorityKey('actor', 'actor-recovery');
  const actorAuthority = await location.readAuthority(actorAuthorityKey);
  assert.equal(actorAuthority.kind, 'snapshot');
  if (actorAuthority.kind !== 'snapshot') return;
  const entrySpotKey = encodeAuthorityKey('user_spot', 'entry-target').value;
  const envelope: ServiceRelocationEnvelope = {
    aggregateId: '22222222-2222-4222-8222-222222222222',
    aggregateGeneration: 1n,
    sourceCleanup: 'pending',
    participants: [{
      key: actorAuthorityKey.value,
      objectKind: 'actor',
      stableType: 'ActorType',
      objectGeneration: actorAuthority.objectGeneration,
      authorityOwnerGeneration: actorAuthority.authorityOwnerGeneration,
      applicationState: Buffer.alloc(0),
      acceptedJournal: Buffer.alloc(0),
      replayCursor: 0n,
      terminalReplies: Buffer.alloc(0),
      pendingRelayCount: 0,
      queuedMessages: [],
      timers: []
    }],
    memberships: [{
      actorKey: actorAuthorityKey.value,
      spotKey: entrySpotKey,
      spotObjectGeneration: 12n,
      membershipEpoch: 1n
    }]
  };
  const targetOwner = { ownerId: 'owner-target', leaseGeneration: 8n };
  const published = await durable.captureAndPublish(
    actorAuthorityKey,
    actorAuthority,
    targetOwner,
    envelope
  );
  const aggregate = await location.prepareAggregate({
    aggregateId: { value: envelope.aggregateId } as never,
    aggregateGeneration: envelope.aggregateGeneration,
    participants: [{
      authorityKey: actorAuthorityKey,
      expectedStoreVersion: published.authority.storeVersion,
      ownerTransition: 'newOwner',
      authorityPayload: published.authority.payload,
      membershipMutation: Buffer.alloc(0)
    }],
    inventoryDigest: createHash('sha256')
      .update(encodeServiceRelocationEnvelope(envelope))
      .digest(),
    targetDescriptor: { meshName: 'mesh-a', rid: 'node-target' as never },
    targetDescriptorLifecycleGeneration: 12n,
    capacity: { actors: 1, spots: 0 },
    targetOwner
  });
  assert.equal(aggregate.kind, 'prepared');
  if (aggregate.kind !== 'prepared') return;
  assert.equal((await location.commitAggregate(aggregate.fence)).kind, 'committed');
  const committed = await location.readAuthority(actorAuthorityKey);
  assert.equal(committed.kind, 'snapshot');
  if (committed.kind !== 'snapshot') return;

  const targetState = {
    spotId: undefined as string | undefined,
    setJoinedSpot: (spotId: string) => {
      targetState.spotId = spotId;
      events.push('membership-restored');
    },
    setLocationGeneration: () => events.push('generation-published'),
    setOwnerLeaseGeneration: () => undefined,
    setRemoteBoundSessionTarget: () => undefined,
    setBoundSessionTransferTarget: () => undefined,
    setBoundSessionBindingGeneration: () => undefined
  };
  const restartedTarget = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map([['mesh-a', {
      actorFactoryRegistrations: {
        ActorType: { implementation: class {}, relocation: { kind: 'recreate' } }
      }
    }]]) },
    locationStore: () => location as never,
    relocationStore: () => relocation as never,
    currentOwner: () => targetOwner,
    liveDescriptors: async () => [],
    meshNode: () => ({
      status: () => ({ routingId: 'node-target', lifecycleGeneration: 12n }),
      sendToNode: () => SubmitResult.Ok
    }),
    completions: () => undefined,
    spotManager: () => ({
      resolveRelocationActivation: () => undefined,
      dispatchRoutedActorPacket: async () => undefined
    }),
    actorManager: () => ({
      prepareRelocationActor: async () => {
        events.push('target-prepared');
        return { context: { actorId: 'actor-recovery' }, configure() {} };
      },
      getState: () => targetState,
      publishRelocationActor: () => events.push('target-published'),
      abortRelocationActor: async () => undefined
    }),
    actorTransfer: {
      publishRoutedActorOwnership: async () => events.push('route-published'),
      openRoutedActorSession: async () => events.push('target-open')
    }
  } as never);

  await restartedTarget.recoverPublishedAuthority(committed);
  const recovered = await location.readAuthority(actorAuthorityKey);
  assert.equal(recovered.kind, 'snapshot');
  if (recovered.kind !== 'snapshot') return;
  assert.equal(codec.read(recovered.payload), undefined);
  assert.deepEqual(events, [
    'target-prepared',
    'membership-restored',
    'generation-published',
    'target-published',
    'route-published',
    'target-open'
  ]);

  await restartedTarget.recoverPublishedAuthority(committed);
  assert.equal(events.filter(event => event === 'target-prepared').length, 1);
});

test('production target selection enforces exact readiness version role capacity and capability', async () => {
  const descriptor = (
    rid: string,
    overrides: Record<string, unknown> = {}
  ): ZLinkMeshNodeDescriptor => ({
    rid,
    lifecycleGeneration: 12n,
    descriptorRevision: 1n,
    ownerId: `owner-${rid}`,
    leaseGeneration: 8n,
    applicationVersion: 2n,
    state: ZLinkFrameworkRuntimeState.Serving,
    objectRole: ZLinkObjectRole.Server,
    placementWeight: 100,
    maintenanceWave: 'green',
    entrySpotId: `entry-${rid}`,
    objectCapabilities: [{
      objectKind: 'actor',
      stableType: 'ActorType',
      policy: 'recreate'
    }],
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 10 },
      spots: { active: 0, reserved: 0, limit: 10 },
      spotTypes: []
    },
    activationConcurrency: { active: 0, limit: 4 },
    ...overrides
  } as never);
  const candidates = [
    descriptor('wrong-version', { applicationVersion: 3n }),
    descriptor('client-only', { objectRole: ZLinkObjectRole.Client }),
    descriptor('zero-weight', { placementWeight: 0 }),
    descriptor('same-wave', { maintenanceWave: 'blue' }),
    descriptor('no-capability', { objectCapabilities: [] }),
    descriptor('actor-full', {
      populationCapacity: {
        actors: { active: 10, reserved: 0, limit: 10 },
        spots: { active: 0, reserved: 0, limit: 10 },
        spotTypes: []
      }
    }),
    descriptor('activation-full', {
      activationConcurrency: { active: 4, limit: 4 }
    }),
    descriptor('stale-peer'),
    descriptor('eligible')
  ];
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map() },
    locationStore: () => undefined,
    relocationStore: () => undefined,
    currentOwner: () => ({ ownerId: 'owner-source', leaseGeneration: 7n }),
    liveDescriptors: async () => candidates,
    localDescriptor: () => ({ applicationVersion: 2n, maintenanceWave: 'blue' } as never),
    meshNode: () => ({
      status: () => ({ routingId: 'node-source', lifecycleGeneration: 11n }),
      peers: () => candidates.map(value => ({
        routingId: value.rid,
        lifecycleGeneration: value.rid === 'stale-peer'
          ? value.lifecycleGeneration - 1n
          : value.lifecycleGeneration,
        state: 3
      }))
    }),
    completions: () => undefined,
    spotManager: () => undefined,
    actorManager: () => undefined,
    actorTransfer: {} as never
  } as never);
  const selected = await (runtime as unknown as {
    selectTarget(
      meshName: string,
      localRid: string,
      targetApplicationVersion: bigint,
      requirements: readonly unknown[]
    ): Promise<ZLinkMeshNodeDescriptor>;
  }).selectTarget('mesh-a', 'node-source', 2n, [{
    objectKind: 'actor',
    stableType: 'ActorType',
    policy: 'recreate',
    count: 1
  }]);

  assert.equal(selected.rid, 'eligible');
});

test('production host relocation inventory applies configured outbound and payload permits concurrently', async () => {
  const activations = Array.from({ length: 9 }, (_, index) => ({
    spotId: `spot-${index}`,
    spot: {},
    spotType: class {}
  }));
  let active = 0;
  let peak = 0;
  let completed = 0;
  const releases: Array<() => void> = [];
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: {
      spotNodes: new Map([['mesh-a', {}]]),
      locations: {
        useInMemoryStores: true,
        options: {
          maxActiveOutboundRelocations: 3,
          maxRelocationPayloadInFlightBytes: 160 * 1024 * 1024
        }
      }
    },
    locationStore: () => undefined,
    relocationStore: () => undefined,
    currentOwner: () => ({ ownerId: 'source-owner', leaseGeneration: 1n }),
    liveDescriptors: async () => [],
    meshNode: () => ({
      status: () => ({ routingId: 'source-node', lifecycleGeneration: 1n })
    }),
    completions: () => undefined,
    spotManager: () => ({
      relocationActivations: () => activations
    }),
    actorManager: () => ({
      snapshotStates: () => []
    }),
    actorTransfer: {} as never
  } as never);
  const internal = runtime as unknown as {
    selectTarget: () => Promise<ZLinkMeshNodeDescriptor>;
    spotKind: () => 'user_spot';
    relocateSpotAggregate: () => Promise<void>;
  };
  internal.selectTarget = async () => ({
    rid: 'target-node',
    lifecycleGeneration: 2n,
    ownerId: 'target-owner',
    leaseGeneration: 2n
  } as never);
  internal.spotKind = () => 'user_spot';
  internal.relocateSpotAggregate = async () => {
    active++;
    peak = Math.max(peak, active);
    await new Promise<void>(resolve => releases.push(resolve));
    active--;
    completed++;
  };

  const operation = runtime.relocateMesh('mesh-a');
  await waitUntil(() => releases.length === 2);
  assert.equal(peak, 2);
  while (completed < activations.length) {
    releases.splice(0).forEach(resolve => resolve());
    await new Promise(resolve => setImmediate(resolve));
  }
  await operation;
  assert.equal(completed, activations.length);
  assert.equal(peak, 2);
});

test('PerActor production inventory relocates the stateless shell before independent Actor units', async () => {
  const activation = {
    spotId: 'per-actor-spot',
    spot: {},
    spotType: class {},
    executionMode: ZLinkUserSpotExecutionMode.PerActor
  };
  const actors = ['actor-a', 'actor-b', 'actor-c'].map(actorId => ({
    actorId,
    actor: {},
    spotId: activation.spotId,
    meshName: 'mesh-a'
  }));
  const events: string[] = [];
  let shellCompleted = false;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: {
      spotNodes: new Map([['mesh-a', {}]]),
      locations: { useInMemoryStores: true, options: {} }
    },
    locationStore: () => ({
      readAuthority: async () => ({ kind: 'snapshot', objectGeneration: 9n })
    }),
    relocationStore: () => undefined,
    currentOwner: () => ({ ownerId: 'source-owner', leaseGeneration: 1n }),
    liveDescriptors: async () => [],
    meshNode: () => ({
      status: () => ({ routingId: 'source-node', lifecycleGeneration: 1n })
    }),
    completions: () => undefined,
    spotManager: () => ({
      relocationActivations: () => [activation]
    }),
    actorManager: () => ({
      snapshotStates: () => actors
    }),
    actorTransfer: {} as never
  } as never);
  const internal = runtime as unknown as {
    selectTarget: () => Promise<ZLinkMeshNodeDescriptor>;
    spotKind: () => 'user_spot';
    relocatePerActorSpotShell: () => Promise<void>;
    relocateStandaloneActor: (
      meshName: string,
      state: { actorId: string },
      target: ZLinkMeshNodeDescriptor,
      targetApplicationVersion: bigint | undefined,
      signal: AbortSignal,
      membership: {
        spotId: string;
        spotKind: ZLinkSpotKind;
        spotObjectGeneration: bigint;
      }
    ) => Promise<void>;
    relocateSpotAggregate: () => Promise<void>;
  };
  internal.selectTarget = async () => ({
    rid: 'target-node',
    lifecycleGeneration: 2n,
    ownerId: 'target-owner',
    leaseGeneration: 2n
  } as never);
  internal.spotKind = () => 'user_spot';
  internal.relocatePerActorSpotShell = async () => {
    events.push('shell');
    shellCompleted = true;
  };
  internal.relocateSpotAggregate = async () => {
    throw new Error('PerActor must not use the SpotWide aggregate path.');
  };
  internal.relocateStandaloneActor = async (
    _mesh,
    state,
    _target,
    _targetApplicationVersion,
    _signal,
    membership
  ) => {
    assert.equal(shellCompleted, true);
    assert.deepEqual(membership, {
      spotId: activation.spotId,
      spotKind: ZLinkSpotKind.User,
      spotObjectGeneration: 9n
    });
    events.push(state.actorId);
  };

  await runtime.relocateMesh('mesh-a');
  assert.equal(events[0], 'shell');
  assert.deepEqual(new Set(events.slice(1)), new Set(actors.map(actor => actor.actorId)));
});

test('production Spot relocation reserves the remote target before sealing source admission', async () => {
  const events: string[] = [];
  const spotType = class {};
  const authority = relocationAuthority('user_spot', 'SpotType');
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: {
      spotNodes: new Map([['mesh-a', {
        spotFactoryRegistrations: {
          SpotType: { implementation: spotType, relocation: { kind: 'recreate' } }
        }
      }]]),
      locations: { useInMemoryStores: true, options: {} }
    },
    locationStore: () => ({
      readAuthority: async () => authority
    }),
    relocationStore: () => ({
      read: async () => ({ kind: 'missing' }),
      delete: async () => {
        events.push('manifest-delete');
        return { kind: 'deleted' };
      }
    }),
    currentOwner: () => ({ ownerId: 'owner-source', leaseGeneration: 7n }),
    liveDescriptors: async () => [],
    meshNode: () => ({
      status: () => ({ routingId: 'node-source', lifecycleGeneration: 11n }),
      sealSpotMessageFollowIngress: () => {
        events.push('spot-follow-seal');
        return { key: 'spot-a\0' + authority.objectGeneration, serial: 1n };
      },
      abortSpotMessageFollowIngress: () => {
        events.push('spot-follow-abort');
        return true;
      },
      commitSpotMessageFollowIngress: async () => true
    }),
    completions: () => undefined,
    spotManager: () => ({
      completeRelocationSource: async () => undefined
    }),
    actorManager: () => ({
      snapshotStates: () => []
    }),
    actorTransfer: {} as never
  } as never);
  const internal = runtime as unknown as {
    preReserveRemote: () => Promise<{
      owner: { abortPreReservation(): Promise<void> };
      manifestRoot: string;
    }>;
    runCoordinator: (...args: readonly unknown[]) => Promise<void>;
    relocateSpotAggregate: (
      meshName: string,
      activation: unknown,
      kind: 'user_spot',
      actors: readonly unknown[],
      target: ZLinkMeshNodeDescriptor,
      targetApplicationVersion?: bigint
    ) => Promise<void>;
  };
  internal.preReserveRemote = async () => {
    events.push('target-reserved');
    return {
      owner: { async abortPreReservation() { events.push('target-abort'); } },
      manifestRoot: 'manifest:preflight'
    };
  };
  internal.runCoordinator = async (...args) => {
    events.push('coordinator');
    await (args[3] as { abortSource(): Promise<void> }).abortSource();
  };
  const activation = {
    spotId: 'spot-a',
    spot: {},
    spotType,
    async captureRelocation() {
      events.push('source-seal');
      return { timers: [] };
    },
    abortRelocation() {
      events.push('source-unseal');
    }
  };
  await internal.relocateSpotAggregate(
    'mesh-a',
    activation,
    'user_spot',
    [],
    {
      rid: 'node-target',
      lifecycleGeneration: 12n,
      ownerId: 'owner-target',
      leaseGeneration: 8n
    } as never,
    undefined
  );
  assert.ok(events.indexOf('target-reserved') < events.indexOf('source-seal'));
  assert.deepEqual(events, [
    'target-reserved',
    'spot-follow-seal',
    'source-seal',
    'coordinator',
    'source-unseal',
    'spot-follow-abort',
    'manifest-delete'
  ]);

  events.length = 0;
  internal.preReserveRemote = async () => {
    events.push('target-rejected');
    throw new Error('target has no permit');
  };
  await assert.rejects(
    internal.relocateSpotAggregate(
      'mesh-a',
      activation,
      'user_spot',
      [],
      {
        rid: 'node-target',
        lifecycleGeneration: 12n,
        ownerId: 'owner-target',
        leaseGeneration: 8n
      } as never,
      undefined
    ),
    /target has no permit/
  );
  assert.deepEqual(events, ['target-rejected']);
});

test('two host owners exchange independent reply relay and ACK with exact durable source fencing', async () => {
  const sourceFence: ServiceWireRequestSourceFence = {
    ownerId: 'owner-source', leaseGeneration: 7n,
    nodeRid: 'node-source', nodeGeneration: 11n
  };
  const coordinator: ServiceWireRelocationCoordinatorFence = {
    ownerId: 'owner-target', leaseGeneration: 8n,
    nodeRid: 'node-target', nodeGeneration: 12n,
    expectedAuthorityStoreVersion: 'version-target'
  };
  const relay: ServiceMaintenanceReplyRelay = {
    relocation: { high: 0x1111111111114111n, low: 0x8111111111111111n },
    targetAttemptGeneration: 1n,
    coordinator,
    operation: { high: 1n, low: 2n },
    replyRouteId: 3n,
    participantId: 1n,
    sequence: 1n,
    terminalResult: 0,
    failureCode: 0,
    payload: {
      packetName: 'zlink.relocation.reply',
      contentType: 'application/json',
      bytes: Buffer.from('{"accepted":true}')
    }
  };
  const relayAuthority: ZLinkAuthoritySnapshot = {
    ...relocationAuthority(),
    storeVersion: { value: 'version-target' } as never,
    payload: new ServiceRelocationAuthorityPayloadCodec().publish(
      Buffer.from('authority-state'),
      {
        phase: 'sourceCleanupCompleted',
        reference: 'completed-root',
        checksumCrc32c: 1,
        aggregateId: '11111111-1111-4111-8111-111111111111',
        aggregateGeneration: 2n,
        inventoryDigest: '0'.repeat(64),
        targetOwnerId: 'owner-target',
        targetOwnerLeaseGeneration: 8n
      }
    )
  };
  const commands: number[] = [];
  let accepted = 0;
  let dropFirstAck = true;
  let corruptSecondAckRoute = true;
  let sourceLeaseExpired = false;
  let staleAdmittedSource = false;
  let staleCoordinatorAuthority = false;
  let source!: ZLinkHostServiceRelocationRuntime;
  let target!: ZLinkHostServiceRelocationRuntime;
  const deliver = async (
    runtime: ZLinkHostServiceRelocationRuntime,
    sourceNodeRid: string,
    payload: Uint8Array
  ): Promise<void> => {
    const part = Message.from(payload);
    try {
      assert.equal(await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid,
        parts: [part]
      } as never), true);
    } finally {
      part.close();
    }
  };
  source = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map() },
    locationStore: () => ({ readAuthority: async () => staleCoordinatorAuthority
      ? { ...relayAuthority, storeVersion: { value: 'stale-version' } as never }
      : relayAuthority }),
    relocationStore: () => undefined,
    currentOwner: () => ({ ownerId: 'owner-source', leaseGeneration: 7n }),
    liveDescriptors: async () => [{ rid: 'node-target', lifecycleGeneration: 12n,
      ownerId: 'owner-target', leaseGeneration: 8n, state: 1 } as never],
    meshNode: () => ({
      status: () => ({ routingId: 'node-source', lifecycleGeneration: 11n }),
      sendToNode: (_nodeRid: string, payload: Uint8Array) => {
        commands.push(payload[3]!);
        if (payload[3] === 46 && dropFirstAck) {
          dropFirstAck = false;
          return SubmitResult.Ok;
        }
        if (payload[3] === 46 && corruptSecondAckRoute) {
          corruptSecondAckRoute = false;
          const ack = decodeMaintenanceReplyRelayAck(payload);
          void deliver(target, 'node-source',
            encodeMaintenanceReplyRelayAck({
              ...ack,
              replyRouteId: ack.replyRouteId + 1n
            }));
          return SubmitResult.Ok;
        }
        void deliver(target, 'node-source', payload);
        return SubmitResult.Ok;
      }
    }),
    completions: () => undefined,
    spotManager: () => undefined,
    actorManager: () => undefined,
    actorTransfer: {
      relayCanonicalMaintenanceTerminal: (
        operationId: string,
        replyRouteId: string,
        result: { readonly ok: boolean; readonly response?: unknown },
        targetNodeRid: string
      ) => {
        if (!['1:2', '1:5', '1:6'].includes(operationId) || replyRouteId !== '3'
          || targetNodeRid !== 'node-target') return { status: 'notAcknowledged' };
        assert.deepEqual(result, { index: 0, ok: true, response: { accepted: true } });
        accepted++;
        return { status: accepted === 1 ? 'terminalReceived' : 'alreadyTerminal', source: {
          ownerId: sourceFence.ownerId,
          ownerLeaseGeneration: sourceFence.leaseGeneration.toString(),
          nodeRid: sourceFence.nodeRid,
          nodeGeneration: sourceFence.nodeGeneration.toString(),
          replyRouteId: '3'
        } };
      }
    }
  } as never);
  (source as unknown as { relocationAuthorityKeys: Map<string, string> })
    .relocationAuthorityKeys.set(
      `${relay.relocation.high}:${relay.relocation.low}`,
      spotKey
    );
  target = new ZLinkHostServiceRelocationRuntime({
    registration: { spotNodes: new Map() },
    locationStore: () => ({
      readOwnerLease: async () => sourceLeaseExpired
        ? { kind: 'missing' as const }
        : { kind: 'found' as const,
            token: { ownerId: 'owner-source', leaseGeneration: 7n },
            leaseExpiresAt: new Date(2_000), storeNow: new Date(1_000) }
    }),
    relocationStore: () => undefined,
    currentOwner: () => ({ ownerId: 'owner-target', leaseGeneration: 8n }),
    liveDescriptors: async () => [{ rid: 'node-source',
      lifecycleGeneration: staleAdmittedSource ? 12n : 11n,
      ownerId: 'owner-source', leaseGeneration: 7n, state: 1 } as never],
    meshNode: () => ({
      status: () => ({ routingId: 'node-target', lifecycleGeneration: 12n }),
      sendToNode: (_nodeRid: string, payload: Uint8Array) => {
        commands.push(payload[3]!);
        void deliver(source, 'node-target', payload);
        return SubmitResult.Ok;
      }
    }),
    completions: () => undefined,
    spotManager: () => undefined,
    actorManager: () => undefined,
    actorTransfer: {} as never
  } as never);
  const sendReplyRelay = (target as unknown as {
    sendReplyRelay(
      meshName: string,
      targetNodeRid: string,
      request: ServiceMaintenanceReplyRelay,
      expectedSource: ServiceWireRequestSourceFence
    ): Promise<'terminalReceived' | 'alreadyTerminal' | 'sourceLeaseExpired'>;
  }).sendReplyRelay.bind(target);

  assert.equal(await sendReplyRelay('mesh-a', 'node-source', relay, sourceFence), 'alreadyTerminal');
  assert.deepEqual(commands, [33, 46, 33, 46, 33, 46]);
  assert.equal(accepted, 3);

  staleAdmittedSource = true;
  await assert.rejects(sendReplyRelay('mesh-a', 'node-source', {
    ...relay,
    operation: { high: 1n, low: 5n }
  }, sourceFence), /exact admitted source fence/);
  staleAdmittedSource = false;

  staleCoordinatorAuthority = true;
  await assert.rejects(
    deliver(source, 'node-target', encodeMaintenanceReplyRelay({
      ...relay,
      operation: { high: 1n, low: 6n }
    })),
    /coordinator authority fence is stale/
  );
  staleCoordinatorAuthority = false;

  await assert.rejects(
    deliver(source, 'node-target', encodeMaintenanceReplyRelay({
      ...relay,
      operation: { high: 1n, low: 3n }
    })),
    /collided/
  );

  sourceLeaseExpired = true;
  const beforeExpiry = commands.length;
  assert.equal(await sendReplyRelay('mesh-a', 'node-source', {
    ...relay,
    operation: { high: 1n, low: 4n }
  }, sourceFence), 'sourceLeaseExpired');
  assert.equal(commands.length, beforeExpiry);
});

test('production host inventory relocates User Spot aggregate Instance Spot and standalone Actor to one remote owner', async () => {
  const events: string[] = [];
  const live = new Set([
    'mesh-a:node-source:11:owner-source:7',
    'mesh-a:node-target:12:owner-target:8'
  ]);
  const location = new ZLinkInMemoryAuthorityStore({
    isTargetLive(descriptor, lifecycle, owner) {
      return live.has(
        `${descriptor.meshName}:${descriptor.rid}:${lifecycle}:${owner.ownerId}:${owner.leaseGeneration}`
      );
    }
  }, () => new Date(100));
  const sourcePlacement = {
    meshName: 'mesh-a',
    nodeRid: 'node-source',
    nodeLifecycleGeneration: 11n,
    owner: { ownerId: 'owner-source', leaseGeneration: 7n }
  };
  await seedProductionAuthority(location, sourcePlacement, 'user_spot', 'room-a', 'RoomSpot');
  await seedProductionAuthority(
    location,
    sourcePlacement,
    'instance_spot',
    'matchmaker-a',
    'MatchmakerSpot'
  );
  await seedProductionAuthority(location, sourcePlacement, 'actor', 'room-actor', 'PlayerActor');
  await seedProductionAuthority(
    location,
    sourcePlacement,
    'actor',
    'standalone-actor',
    'PlayerActor'
  );

  class RoomSpot {}
  class MatchmakerSpot {}
  class PlayerActor {}
  class ProductionRelocationAdapter {
    async capture(value: { readonly identity: string }, _signal: AbortSignal) {
      events.push(`capture:${value.identity}`);
      return Buffer.from(`state:${value.identity}`);
    }

    async restore(
      value: { readonly identity: string },
      payload: Uint8Array,
      _signal: AbortSignal
    ) {
      assert.equal(Buffer.from(payload).toString(), `state:${value.identity}`);
      events.push(`restore:${value.identity}`);
    }
  }
  const registration = {
    spotNodes: new Map([['mesh-a', {
      spotFactoryRegistrations: {
        RoomSpot: {
          implementation: RoomSpot,
          relocation: { kind: 'snapshot', adapterType: ProductionRelocationAdapter }
        }
      },
      instanceSpotFactoryRegistrations: {
        MatchmakerSpot: {
          implementation: MatchmakerSpot,
          relocation: { kind: 'snapshot', adapterType: ProductionRelocationAdapter }
        }
      },
      actorFactoryRegistrations: {
        PlayerActor: {
          implementation: PlayerActor,
          relocation: { kind: 'snapshot', adapterType: ProductionRelocationAdapter }
        }
      }
    }]]),
    locations: {
      useInMemoryStores: true,
      options: {
        maxActiveOutboundRelocations: 3,
        maxActiveInboundRelocations: 3,
        maxRelocationPayloadInFlightBytes: 256 * 1024 * 1024
      }
    }
  };
  const targetDescriptor = productionRelocationDescriptor();
  const relocation = new MemoryPublicRelocationStore();
  const roomActor = { context: { actorId: 'room-actor' }, identity: 'room-actor' };
  const standaloneActor = {
    context: { actorId: 'standalone-actor' },
    identity: 'standalone-actor'
  };
  const roomActivation = productionSourceActivation(
    'room-a',
    new RoomSpot() as RoomSpot & { identity: string },
    RoomSpot,
    events,
    true
  );
  (roomActivation.spot as RoomSpot & { identity: string }).identity = 'room-a';
  const matchmakerActivation = productionSourceActivation(
    'matchmaker-a',
    new MatchmakerSpot() as MatchmakerSpot & { identity: string },
    MatchmakerSpot,
    events,
    false
  );
  (matchmakerActivation.spot as MatchmakerSpot & { identity: string }).identity =
    'matchmaker-a';
  const sourceStates = [
    productionSourceActorState(roomActor, 'room-a'),
    productionSourceActorState(standaloneActor, undefined)
  ];
  const replayPacket = productionQueuedPacket();
  const targetActors = new Map<string, { context: { actorId: string }; identity: string }>();
  const targetActorStates = new Map<string, {
    spotId?: string;
    setJoinedSpot(spotId: string): void;
    setLocationGeneration(generation: bigint): void;
    setOwnerLeaseGeneration(generation: bigint): void;
    setRemoteBoundSessionTarget(target: unknown): void;
    setBoundSessionTransferTarget(target: unknown): void;
    setBoundSessionBindingGeneration(generation: bigint): void;
  }>();
  const targetSpots = new Map<string, {
    spotId: string;
    spot: { identity: string };
    timers: { restoreRelocation(values: readonly unknown[]): void };
    commitActorJoin(actor: { context: { actorId: string } }): void;
  }>();
  let source!: ZLinkHostServiceRelocationRuntime;
  let target!: ZLinkHostServiceRelocationRuntime;
  const deliver = async (
    runtime: ZLinkHostServiceRelocationRuntime,
    sourceNodeRid: string,
    payload: Uint8Array
  ): Promise<void> => {
    const part = Message.from(payload);
    try {
      assert.equal(await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid,
        parts: [part]
      } as never), true);
    } finally {
      part.close();
    }
  };
  const targetMeshNode = {
    status: () => ({ routingId: 'node-target', lifecycleGeneration: 12n }),
    peers: () => [{
      routingId: 'node-source',
      lifecycleGeneration: 11n,
      state: 3
    }],
    sendToNode: (nodeRid: string, payload: Uint8Array) => {
      assert.equal(String(nodeRid), 'node-source');
      void deliver(source, 'node-target', payload);
      return SubmitResult.Ok;
    }
  };
  target = new ZLinkHostServiceRelocationRuntime({
    registration,
    locationStore: () => location as never,
    relocationStore: () => relocation as never,
    currentOwner: () => ({ ownerId: 'owner-target', leaseGeneration: 8n }),
    liveDescriptors: async () => [targetDescriptor],
    localDescriptor: () => targetDescriptor,
    meshNode: () => targetMeshNode,
    completions: () => undefined,
    spotManager: () => ({
      prepareRelocationSpot: async (
        _meshName: string,
        _kind: string,
        _stableType: string,
        implementation: new () => unknown,
        spotId: string
      ) => {
        const spot = Object.assign(new implementation() as object, {
          identity: String(spotId)
        }) as { identity: string };
        const activation = {
          spotId: String(spotId),
          spot,
          timers: {
            restoreRelocation(values: readonly unknown[]) {
              events.push(`timer-restored:${String(spotId)}:${values.length}`);
            }
          },
          commitActorJoin(actor: { context: { actorId: string } }) {
            events.push(`membership-restored:${actor.context.actorId}:${String(spotId)}`);
          },
          async notifyRelocatedBoundary() {}
        };
        targetSpots.set(String(spotId), activation);
        events.push(`hidden-spot:${String(spotId)}`);
        return activation;
      },
      resolveRelocationActivation: (_meshName: string, spotId: string) =>
        targetSpots.get(String(spotId)),
      publishRelocationSpot: async (activation: { spotId: string }) => {
        events.push(`published-spot:${String(activation.spotId)}`);
      },
      abortRelocationSpot: async () => undefined,
      dispatchRoutedActorPacket: async (
        _spotId: string,
        actorId: string
      ) => {
        events.push(`queue-replayed:${actorId}`);
        return { accepted: true };
      }
    }),
    actorManager: () => ({
      prepareRelocationActor: async (
        actorId: string,
        _stableType: string
      ) => {
        const actor = { context: { actorId }, identity: actorId };
        const state = {
          spotId: undefined as string | undefined,
          setJoinedSpot(spotId: string) {
            state.spotId = String(spotId);
          },
          setLocationGeneration(generation: bigint) {
            events.push(`generation-published:${actorId}:${generation}`);
          },
          setOwnerLeaseGeneration() {},
          setRemoteBoundSessionTarget() {
            events.push(`session-restored:${actorId}`);
          },
          setBoundSessionTransferTarget() {},
          setBoundSessionBindingGeneration() {}
        };
        targetActors.set(actorId, actor);
        targetActorStates.set(actorId, state);
        events.push(`hidden-actor:${actorId}`);
        return actor;
      },
      getState: (actorId: string) => targetActorStates.get(actorId),
      publishRelocationActor: (actorId: string) => {
        events.push(`published-actor:${actorId}`);
      },
      abortRelocationActor: async () => undefined
    }),
    actorTransfer: {
      publishRoutedActorOwnership: async (actor: { context: { actorId: string } }) => {
        events.push(`session-route-ack:${actor.context.actorId}`);
      },
      openRoutedActorSession: async (actor: { context: { actorId: string } }) => {
        events.push(`admission-open:${actor.context.actorId}`);
      }
    }
  } as never);
  const sourceMeshNode = {
    status: () => ({ routingId: 'node-source', lifecycleGeneration: 11n }),
    peers: () => [{
      routingId: 'node-target',
      lifecycleGeneration: 12n,
      state: 3
    }],
    sendToNode: (nodeRid: string, payload: Uint8Array) => {
      assert.equal(String(nodeRid), 'node-target');
      void deliver(target, 'node-source', payload);
      return SubmitResult.Ok;
    },
    sealSpotMessageFollowIngress: (request: { spot: { spotId: string } }) => {
      events.push(`follow-sealed:${request.spot.spotId}`);
      return { key: request.spot.spotId, serial: 1n };
    },
    abortSpotMessageFollowIngress: () => true,
    commitSpotMessageFollowIngress: async (
      seal: { key: string },
      _target: unknown,
      _durationMs: number
    ) => {
      events.push(`follow-committed:${seal.key}`);
      return true;
    }
  };
  source = new ZLinkHostServiceRelocationRuntime({
    registration,
    locationStore: () => location as never,
    relocationStore: () => relocation as never,
    currentOwner: () => ({ ownerId: 'owner-source', leaseGeneration: 7n }),
    liveDescriptors: async () => [targetDescriptor],
    localDescriptor: () => ({
      applicationVersion: 7n,
      maintenanceWave: 'blue'
    } as never),
    meshNode: () => sourceMeshNode,
    completions: () => undefined,
    spotManager: () => ({
      relocationActivations: () => [roomActivation, matchmakerActivation],
      completeRelocationSource: async (activation: { spotId: string }) => {
        events.push(`source-spot-cleanup:${String(activation.spotId)}`);
      }
    }),
    actorManager: () => ({
      snapshotStates: () => sourceStates,
      completeRelocationSource: async (actorId: string) => {
        events.push(`source-actor-cleanup:${actorId}`);
      }
    }),
    actorTransfer: {
      prepareMaintenanceSession: async (actor: { context: { actorId: string } }) => {
        const actorId = actor.context.actorId;
        events.push(`source-session-sealed:${actorId}`);
        return {
          target: {
            routerChannelId: 'mesh-a',
            targetNodeRid: 'node-source',
            spotId: actorId === 'room-actor' ? 'room-a' : 'entry-source',
            sessionNodeRid: 'session-node',
            sessionRid: `session-${actorId}`,
            bindingGeneration: 3n
          },
          handoffBacklog: actorId === 'room-actor' ? [replayPacket] : [],
          setReplayResults: () => undefined,
          commit: async () => events.push(`source-session-commit:${actorId}`),
          rollback: async () => events.push(`source-session-rollback:${actorId}`)
        };
      }
    }
  } as never);

  await source.relocateMesh('mesh-a', 7n);

  for (const [kind, id] of [
    ['user_spot', 'room-a'],
    ['instance_spot', 'matchmaker-a'],
    ['actor', 'room-actor'],
    ['actor', 'standalone-actor']
  ] as const) {
    const authority = await location.readAuthority(encodeAuthorityKey(kind, id));
    assert.equal(authority.kind, 'snapshot');
    if (authority.kind !== 'snapshot') continue;
    assert.equal(authority.ownerId, 'owner-target');
    assert.equal(authority.ownerLeaseGeneration, 8n);
  }
  assert.ok(events.includes('restore:room-a'));
  assert.ok(events.includes('restore:matchmaker-a'));
  assert.ok(events.includes('restore:room-actor'));
  assert.ok(events.includes('restore:standalone-actor'));
  assert.ok(events.includes('membership-restored:room-actor:room-a'));
  assert.ok(events.includes('queue-replayed:room-actor'));
  assert.ok(events.includes('timer-restored:room-a:1'));
  for (const actorId of ['room-actor', 'standalone-actor']) {
    assert.ok(
      events.indexOf(`source-actor-cleanup:${actorId}`)
        < events.indexOf(`session-route-ack:${actorId}`)
    );
    assert.ok(
      events.indexOf(`session-route-ack:${actorId}`)
        < events.indexOf(`admission-open:${actorId}`)
    );
  }
  assert.ok(
    events.indexOf('queue-replayed:room-actor')
      < events.indexOf('published-actor:room-actor')
  );
  assert.ok(
    events.indexOf('timer-restored:room-a:1')
      < events.indexOf('published-spot:room-a')
  );
  assert.ok(events.includes('follow-committed:room-a'));
  assert.ok(events.includes('follow-committed:matchmaker-a'));
});

function relocationPrepare(
  envelope: ServiceRelocationEnvelope,
  root: { readonly reference: string; readonly checksumCrc32c: number }
): ServiceMaintenanceRelocationPrepare {
  const coordinator: ServiceWireRelocationCoordinatorFence = {
    ownerId: 'owner-coordinator', leaseGeneration: 9n, nodeRid: 'node-coordinator',
    nodeGeneration: 13n, expectedAuthorityStoreVersion: 'version-a'
  };
  const candidate: ServiceWireRelocationCandidate = {
    nodeRid: 'node-target', nodeGeneration: 12n,
    ownerId: 'owner-target', ownerLeaseGeneration: 8n
  };
  const object: ServiceWireRelocationObject = {
    kind: 'userSpot', spotId: 'spot-a', objectGeneration: 1n,
    expectedAuthorityOwnerGeneration: 1n
  };
  const hasAcceptedRecord = envelope.participants[1]!.queuedMessages.length !== 0;
  const acceptedBytes = hasAcceptedRecord
    ? acceptedFrozenRecord(envelope, coordinator, candidate).canonicalBytes.byteLength
    : 0;
  const participants: readonly ServiceWireRelocationParticipant[] = [
    { participantId: 1n, allowanceMessages: hasAcceptedRecord ? 1n : 0n,
      allowanceBytes: BigInt(acceptedBytes) },
    { participantId: 2n, allowanceMessages: 0n, allowanceBytes: 0n }
  ];
  return { kind: 'prepare', relocation: { high: 0x1111111111114111n, low: 0x8111111111111111n },
    targetAttemptGeneration: envelope.aggregateGeneration, round: 'initial', coordinator,
    candidate, initiatorRole: 'source', object, sourceNodeRid: 'node-source',
    sourceNodeGeneration: 11n, requiredMessages: hasAcceptedRecord ? 1n : 0n,
    requiredBytes: BigInt(acceptedBytes),
    participants, root, applicationVersion: 1n };
}

function sourceFence() {
  return { ownerId: 'owner-source', leaseGeneration: 7n,
    nodeRid: 'node-source', nodeGeneration: 11n };
}

function acceptedFrozenRecord(
  envelope: ServiceRelocationEnvelope,
  coordinator: ServiceWireRelocationCoordinatorFence,
  candidate: ServiceWireRelocationCandidate
) {
  const participant = envelope.participants[1]!;
  const message = participant.queuedMessages[0]!;
  return encodeServiceWireFrozenActorApplicationRecord({
    source: {
      ownerId: coordinator.ownerId,
      leaseGeneration: coordinator.leaseGeneration,
      nodeRid: coordinator.nodeRid,
      nodeGeneration: coordinator.nodeGeneration
    },
    target: {
      actorId: 'actor-a',
      objectGeneration: participant.objectGeneration,
      nodeRid: candidate.nodeRid,
      nodeGeneration: candidate.nodeGeneration,
      authorityOwnerGeneration: participant.authorityOwnerGeneration + 1n,
      ownerLeaseGeneration: candidate.ownerLeaseGeneration
    },
    operationId: acceptedOperation,
    payload: {
      packetName: '__zlink.actor.handoff.accepted',
      contentType: 'application/json',
      bytes: message.payload
    }
  });
}

function relocationEnvelope(): ServiceRelocationEnvelope {
  const header = Buffer.from('header');
  const payload = Buffer.from('payload');
  const parts = [Message.from(header), Message.from(payload)];
  const queuedOwner = ownerFence({
    ownerId: 'owner-source',
    ownerLeaseGeneration: 7n,
    nodeRid: 'node-source',
    nodeGeneration: 11n,
    authorityOwnerGeneration: 1n
  });
  const messageFollowContext = {
    operationId: acceptedOperationId,
    objectGeneration: '1',
    sourceOwner: queuedOwner,
    targetOwner: queuedOwner,
    request: false,
    hopCount: 0,
    visitedOwners: [messageFollowOwnerFenceKey(queuedOwner)],
    payloadChecksumSha256: actorMessageFollowPayloadChecksum(parts)
  };
  parts.forEach(part => part.close());
  return { aggregateId: '11111111-1111-4111-8111-111111111111', aggregateGeneration: 1n,
    sourceCleanup: 'pending', participants: [
      { key: spotKey, objectKind: 'user_spot', stableType: 'SpotType',
        objectGeneration: 1n, authorityOwnerGeneration: 1n,
        applicationState: Buffer.alloc(0), acceptedJournal: Buffer.alloc(0), replayCursor: 0n,
        terminalReplies: Buffer.alloc(0), pendingRelayCount: 0, queuedMessages: [], timers: [] },
      { key: actorKey, objectKind: 'actor', stableType: 'ActorType',
        objectGeneration: 1n, authorityOwnerGeneration: 1n,
        applicationState: Buffer.alloc(0), acceptedJournal: Buffer.alloc(0), replayCursor: 0n,
        terminalReplies: Buffer.alloc(0), pendingRelayCount: 0,
        queuedMessages: [{ sequence: 1n, payload: Buffer.from(JSON.stringify({
          index: 0,
          header: header.toString('base64'),
          payload: payload.toString('base64'),
          returnResponse: false,
          operationId: `${acceptedOperation.high}:${acceptedOperation.low}`,
          messageFollowContext
        })) }], timers: [] }
    ], memberships: [{ actorKey, spotKey, spotObjectGeneration: 1n, membershipEpoch: 1n }] };
}

function relocationAuthority(
  objectKind: 'actor' | 'user_spot' = 'user_spot',
  stableType = 'SpotType',
  payload: Uint8Array = Buffer.from('authority-state')
): ZLinkAuthoritySnapshot {
  return { kind: 'snapshot', storeVersion: { value: 'version-a' } as never,
    payload, objectGeneration: 1n,
    authorityOwnerGeneration: 1n, ownerId: 'owner-source', ownerLeaseGeneration: 7n,
    allocation: { state: 'active', objectKind, stableType,
      descriptor: { meshName: 'mesh-a', rid: 'node-source' as never },
      descriptorLifecycleGeneration: 11n,
      capacity: objectKind === 'actor' ? { actors: 1, spots: 0 } : { actors: 0, spots: 1 } },
    storeNow: new Date(1) };
}

function foundBlob(bytes: Uint8Array) {
  const storeNow = new Date();
  return {
    kind: 'found' as const,
    bytes,
    expiresAt: new Date(storeNow.getTime() + 60_000),
    storeNow
  };
}

class MemoryPublicRelocationStore {
  private readonly values = new Map<string, {
    readonly bytes: Buffer;
    expiresAt: Date;
  }>();

  async put(
    reference: { readonly value: string },
    payload: Uint8Array,
    retentionMs: number
  ) {
    const storeNow = new Date(100);
    const existing = this.values.get(reference.value);
    if (existing !== undefined) {
      return existing.bytes.equals(Buffer.from(payload))
        ? { kind: 'alreadyStored' as const, expiresAt: existing.expiresAt, storeNow }
        : { kind: 'conflict' as const, storeNow };
    }
    const expiresAt = new Date(storeNow.getTime() + retentionMs);
    this.values.set(reference.value, { bytes: Buffer.from(payload), expiresAt });
    return { kind: 'stored' as const, expiresAt, storeNow };
  }

  async read(reference: { readonly value: string }) {
    const storeNow = new Date(100);
    const existing = this.values.get(reference.value);
    return existing === undefined
      ? { kind: 'missing' as const, storeNow }
      : {
          kind: 'found' as const,
          bytes: Buffer.from(existing.bytes),
          expiresAt: existing.expiresAt,
          storeNow
        };
  }

  async renew(reference: { readonly value: string }, retentionMs: number) {
    const storeNow = new Date(100);
    const existing = this.values.get(reference.value);
    if (existing === undefined) return { kind: 'missing' as const, storeNow };
    existing.expiresAt = new Date(storeNow.getTime() + retentionMs);
    return { kind: 'renewed' as const, expiresAt: existing.expiresAt, storeNow };
  }

  async delete(reference: { readonly value: string }): Promise<void> {
    this.values.delete(reference.value);
  }

  asRuntimePort() {
    return {
      put: async (payload: Uint8Array, retentionMs: number) => {
        const reference = createHash('sha256').update(payload).digest('hex');
        const result = await this.put({ value: reference }, payload, retentionMs);
        if (result.kind === 'conflict') {
          throw new Error('Relocation test Store reference collided.');
        }
        return {
          reference,
          checksumCrc32c: crc32c(payload),
          expiresAtMs: result.expiresAt.getTime(),
          storeNowMs: result.storeNow.getTime()
        };
      },
      get: async (reference: string) => {
        const result = await this.read({ value: reference });
        return result.kind === 'missing'
          ? { kind: 'missing' as const, storeNowMs: result.storeNow.getTime() }
          : {
              kind: 'found' as const,
              payload: result.bytes,
              expiresAtMs: result.expiresAt.getTime(),
              storeNowMs: result.storeNow.getTime()
            };
      },
      delete: async (reference: string) =>
        this.values.delete(reference) ? 'deleted' as const : 'missing' as const
    };
  }
}

async function seedProductionAuthority(
  store: ZLinkInMemoryAuthorityStore,
  target: {
    readonly meshName: string;
    readonly nodeRid: string;
    readonly nodeLifecycleGeneration: bigint;
    readonly owner: { readonly ownerId: string; readonly leaseGeneration: bigint };
  },
  kind: 'actor' | 'user_spot' | 'instance_spot',
  globalId: string,
  stableType: string
): Promise<void> {
  const reserved = await store.reserve({
    key: { kind, globalId },
    intent: {
      stableType,
      requestContentReference: `create:${kind}:${globalId}`,
      requestSha256: createHash('sha256').update(`${kind}:${globalId}`).digest(),
      requestEncodedSize: 1n
    },
    target: target as never,
    creatingPayload: Buffer.from(`creating:${globalId}`),
    capacity: kind === 'actor'
      ? { actors: 1, spots: 0 }
      : {
          actors: 0,
          spots: 1,
          spotType: { objectKind: kind, stableType, count: 1 }
        }
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  if (kind === 'actor') {
    const terminal = Buffer.from(`created:${kind}:${globalId}`);
    const completed = await store.completeCreation({
      key: { kind, globalId },
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target: target as never,
      completion: {
        kind: 'created',
        readyPayload: Buffer.from(`ready:${globalId}`),
        terminal: {
          operation: {
            sourceNodeRid: target.nodeRid,
            sourceNodeGeneration: target.nodeLifecycleGeneration,
            operationId: {
              high: 1n,
              low: BigInt(globalId.length)
            }
          },
          terminalEnvelope: terminal,
          terminalEnvelopeSha256: createHash('sha256').update(terminal).digest(),
          operationDeadline: new Date(10_000)
        }
      }
    });
    assert.equal(completed.kind, 'created');
    return;
  }
  const completed = await store.commit({
    key: { kind, globalId },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target: target as never,
    readyPayload: Buffer.from(`ready:${globalId}`)
  });
  assert.equal(completed.kind, 'committed');
}

function productionRelocationDescriptor(): ZLinkMeshNodeDescriptor {
  return {
    rid: 'node-target',
    lifecycleGeneration: 12n,
    descriptorRevision: 1n,
    ownerId: 'owner-target',
    leaseGeneration: 8n,
    applicationVersion: 7n,
    state: ZLinkFrameworkRuntimeState.Serving,
    objectRole: ZLinkObjectRole.Server,
    placementWeight: 100,
    maintenanceWave: 'green',
    entrySpotId: 'entry-target',
    objectCapabilities: [
      { objectKind: 'user_spot', stableType: 'RoomSpot', policy: 'snapshot' },
      { objectKind: 'instance_spot', stableType: 'MatchmakerSpot', policy: 'snapshot' },
      { objectKind: 'actor', stableType: 'PlayerActor', policy: 'snapshot' }
    ],
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 100 },
      spots: { active: 0, reserved: 0, limit: 100 },
      spotTypes: [
        {
          objectKind: 'user_spot',
          stableType: 'RoomSpot',
          active: 0,
          reserved: 0,
          limit: 100
        },
        {
          objectKind: 'instance_spot',
          stableType: 'MatchmakerSpot',
          active: 0,
          reserved: 0,
          limit: 100
        }
      ]
    },
    activationConcurrency: { active: 0, limit: 8 }
  } as never;
}

function productionSourceActivation<T extends object>(
  spotId: string,
  spot: T,
  spotType: new () => T,
  events: string[],
  withTimer: boolean
) {
  return {
    spotId,
    spot,
    spotType,
    executionMode: ZLinkUserSpotExecutionMode.SpotWide,
    async captureRelocation() {
      events.push(`source-spot-sealed:${spotId}`);
      return {
        timers: withTimer
          ? [{
              name: 'round',
              periodMs: 1_000,
              overrunPolicy: ZLinkTimerOverrunPolicy.SkipLateTicks,
              maxCatchUpTicks: 1,
              stopOnUnhandledException: false,
              startedAtUnixMs: 100,
              deliveryIndex: 2n,
              lastScheduledIndex: 3n,
              pendingTicks: 1
            }]
          : []
      };
    },
    async commitRelocation() {
      events.push(`source-spot-committed:${spotId}`);
      return true;
    },
    abortRelocation() {
      events.push(`source-spot-aborted:${spotId}`);
      return true;
    },
    commitActorDeparture(actorId: string) {
      events.push(`source-membership-cleanup:${actorId}:${spotId}`);
    },
    async completeConsumedRelocationBoundary() {}
  };
}

function productionSourceActorState(
  actor: { readonly context: { readonly actorId: string }; readonly identity: string },
  spotId: string | undefined
) {
  return {
    actorId: actor.context.actorId,
    actor,
    actorType: 'PlayerActor',
    meshName: 'mesh-a',
    spotId,
    spotMembershipEpoch: 4n
  };
}

function productionQueuedPacket() {
  const header = Buffer.from('queued-header');
  const payload = Buffer.from('queued-payload');
  const parts = [Message.from(header), Message.from(payload)];
  const queuedOwner = ownerFence({
    ownerId: 'owner-source',
    ownerLeaseGeneration: 7n,
    nodeRid: 'node-source',
    nodeGeneration: 11n,
    authorityOwnerGeneration: 1n
  });
  const result = {
    index: 0,
    header: header.toString('base64'),
    payload: payload.toString('base64'),
    returnResponse: false,
    messageFollowContext: {
      operationId: acceptedOperationId,
      objectGeneration: '1',
      sourceOwner: queuedOwner,
      targetOwner: queuedOwner,
      request: false,
      hopCount: 0,
      visitedOwners: [messageFollowOwnerFenceKey(queuedOwner)],
      payloadChecksumSha256: actorMessageFollowPayloadChecksum(parts)
    }
  };
  parts.forEach(part => part.close());
  return result;
}

async function waitUntil(predicate: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 1000; attempt++) {
    if (predicate()) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  throw new Error('Timed out waiting for production relocation permits.');
}
