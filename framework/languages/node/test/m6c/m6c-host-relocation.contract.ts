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
import {
  createServiceRelocationId,
  ZLinkHostServiceRelocationRuntime
} from '../../packages/framework/src/runtime/host/service-relocation-host-runtime';
import {
  encodeServiceRelocationControlRequest,
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
  decodeActorAuthorityIdentity,
  encodeActorAuthorityIdentity
} from '../../packages/framework/src/runtime/actors/actor-authority-publication';
import {
  decodeSessionRelocationRouted,
  decodeSessionRelocationSealed,
  decodeMaintenanceReplyRelayAck,
  encodeSessionRelocationRoute,
  encodeSessionRelocationRouted,
  encodeSessionRelocationSeal,
  encodeSessionRelocationSealed,
  encodeMaintenanceReplyRelay,
  encodeMaintenanceReplyRelayAck,
  encodeServiceWireFrozenActorApplicationRecord,
  type ServiceMaintenanceReplyRelay,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationRouted,
  type ServiceSessionRelocationSeal,
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

test('production host dispatches binary commands 42 and 44 and sends canonical 43 and 45 ACKs', async () => {
  const authority = (
    nodeRid: string,
    nodeGeneration: bigint,
    authorityOwnerGeneration: bigint,
    ownerId: string,
    ownerLeaseGeneration: bigint,
    storeVersion: string
  ) => ({
    kind: 'snapshot' as const,
    storeVersion: { value: storeVersion },
    payload: Buffer.alloc(0),
    objectGeneration: 5n,
    authorityOwnerGeneration,
    ownerId,
    ownerLeaseGeneration,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'mesh-a', rid: nodeRid },
      descriptorLifecycleGeneration: nodeGeneration,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date()
  });
  let currentAuthority = authority('source', 2n, 11n, 'coordinator', 3n, 'store-v17');
  let targetDescriptorLeaseGeneration = 14n;
  const sent: Array<{ readonly target: string; readonly bytes: Buffer }> = [];
  const received: string[] = [];
  const seal = {
    relocation: { high: 7n, low: 9n },
    coordinator: {
      ownerId: 'coordinator', leaseGeneration: 3n,
      nodeRid: 'source', nodeGeneration: 2n,
      expectedAuthorityStoreVersion: 'store-v17'
    },
    senderRole: 'source' as const,
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
      sessionRid: 'session',
      bindingGeneration: 6n
    }
  };
  const route = {
    relocation: seal.relocation,
    coordinator: seal.coordinator,
    senderRole: 'target' as const,
    actor: { nodeRid: 'target', actorId: 'actor-1', generation: 5n },
    session: seal.session,
    route: {
      action: 'commit' as const,
      previousAuthorityOwnerGeneration: 11n,
      targetAuthorityOwnerGeneration: 12n,
      targetNodeRid: 'target',
      targetNodeGeneration: 6n,
      replayedHighWater: 41n
    }
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => ({ readAuthority: async () => currentAuthority } as never),
    liveDescriptors: async () => [{
      rid: 'target', lifecycleGeneration: 6n,
      ownerId: 'target-owner', leaseGeneration: targetDescriptorLeaseGeneration
    }],
    currentOwner: () => ({ ownerId: 'session-owner-id', leaseGeneration: 8n }),
    meshNode: () => ({
      status: () => ({ routingId: 'session-owner', lifecycleGeneration: 4n }),
      peers: () => [
        { routingId: 'source', lifecycleGeneration: 2n, state: 3 },
        { routingId: 'target', lifecycleGeneration: 6n, state: 3 }
      ],
      sendToNode: (target: string, bytes: Uint8Array) => {
        sent.push({ target, bytes: Buffer.from(bytes) });
        return SubmitResult.Ok;
      }
    }),
    boundSessionRelocation: {
      receiveSeal: async (value: ServiceSessionRelocationSeal) => {
        received.push('seal');
        assert.deepEqual(value, seal);
        return {
          relocation: value.relocation,
          coordinator: value.coordinator,
          actor: value.actor,
          session: value.session,
          lastAcceptedSessionSequence: 41n
        };
      },
      receiveRoute: async (
        value: ServiceSessionRelocationRoute,
        targetOwnerLeaseGeneration?: bigint
      ) => {
        received.push('route');
        assert.deepEqual(value, { ...route, actor: { ...route.actor, nodeRid: '' } });
        assert.equal(targetOwnerLeaseGeneration, 14n);
        return {
          relocation: value.relocation,
          coordinator: value.coordinator,
          actor: value.actor,
          session: value.session,
          action: value.route.action,
          result: 'applied',
          currentAuthorityOwnerGeneration: 12n,
          lastAcceptedSessionSequence: 41n
        };
      }
    }
  } as never);
  const dispatch = async (sourceNodeRid: string, bytes: Uint8Array) => {
    const part = Message.from(bytes);
    try {
      assert.equal(await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid,
        parts: [part]
      } as never), true);
    } finally {
      part.close();
    }
  };

  await dispatch('source', encodeSessionRelocationSeal(seal));
  assert.deepEqual(received, ['seal']);
  assert.equal(sent[0]?.target, 'source');
  assert.equal(decodeSessionRelocationSealed(sent[0]!.bytes)
    .lastAcceptedSessionSequence, 41n);

  currentAuthority = authority('target', 6n, 12n, 'target-owner', 14n, 'store-v18');
  await dispatch('target', encodeSessionRelocationRoute(route));
  assert.deepEqual(received, ['seal', 'route']);
  assert.equal(sent[1]?.target, 'target');
  assert.equal(decodeSessionRelocationRouted(sent[1]!.bytes).result, 'applied');
  targetDescriptorLeaseGeneration = 15n;
  await assert.rejects(
    dispatch('target', encodeSessionRelocationRoute(route)),
    /current target authority/
  );
  await runtime.dispose();
});

test('session relocation requests single-flight exact bytes and validate late command 43 and 45 ACKs', async () => {
  const seal: ServiceSessionRelocationSeal = {
    relocation: { high: 7n, low: 9n },
    coordinator: {
      ownerId: 'coordinator', leaseGeneration: 3n,
      nodeRid: 'source', nodeGeneration: 2n,
      expectedAuthorityStoreVersion: 'store-v17'
    },
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
      sessionRid: 'session',
      bindingGeneration: 6n
    }
  };
  const route: ServiceSessionRelocationRoute = {
    relocation: seal.relocation,
    coordinator: seal.coordinator,
    senderRole: 'target',
    actor: { nodeRid: 'target', actorId: 'actor-1', generation: 5n },
    session: seal.session,
    route: {
      action: 'commit',
      previousAuthorityOwnerGeneration: 11n,
      targetAuthorityOwnerGeneration: 12n,
      targetNodeRid: 'target',
      targetNodeGeneration: 6n,
      replayedHighWater: 41n
    }
  };
  const sent: Buffer[] = [];
  const runtime = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => ({
      sendToNode: (_target: string, bytes: Uint8Array) => {
        sent.push(Buffer.from(bytes));
        return SubmitResult.Ok;
      }
    })
  } as never);
  const dispatchAck = async (bytes: Uint8Array) => {
    const part = Message.from(bytes);
    try {
      return await runtime.tryHandleControl('mesh-a', {
        sourceNodeRid: 'session-owner',
        parts: [part]
      } as never);
    } finally {
      part.close();
    }
  };
  try {
    const firstSeal = runtime.requestSessionRelocationSeal(
      'mesh-a', 'session-owner', seal
    );
    const duplicateSeal = runtime.requestSessionRelocationSeal(
      'mesh-a', 'session-owner', seal
    );
    assert.equal(firstSeal, duplicateSeal);
    assert.equal(sent.length, 1);
    await assert.rejects(
      runtime.requestSessionRelocationSeal('mesh-a', 'session-owner', {
        ...seal,
        coordinator: { ...seal.coordinator, expectedAuthorityStoreVersion: 'different' }
      }),
      error => error instanceof Error && /different bytes or target/.test(error.message)
    );
    const sealed = {
      relocation: seal.relocation,
      coordinator: seal.coordinator,
      actor: seal.actor,
      session: seal.session,
      lastAcceptedSessionSequence: 41n
    };
    assert.equal(await dispatchAck(encodeSessionRelocationSealed(sealed)), true);
    assert.deepEqual(await firstSeal, sealed);
    assert.equal(await dispatchAck(encodeSessionRelocationSealed(sealed)), true);
    await assert.rejects(
      dispatchAck(encodeSessionRelocationSealed({
        ...sealed,
        lastAcceptedSessionSequence: 42n
      })),
      error => error instanceof Error && /different bytes/.test(error.message)
    );
    const retriedSeal = runtime.requestSessionRelocationSeal(
      'mesh-a', 'session-owner', seal
    );
    assert.equal(sent.length, 2);
    assert.equal(await dispatchAck(encodeSessionRelocationSealed(sealed)), true);
    assert.deepEqual(await retriedSeal, sealed);

    const firstRoute = runtime.requestSessionRelocationRoute(
      'mesh-a', 'session-owner', route
    );
    const duplicateRoute = runtime.requestSessionRelocationRoute(
      'mesh-a', 'session-owner', route
    );
    assert.equal(firstRoute, duplicateRoute);
    assert.equal(sent.length, 3);
    const routed = {
      relocation: route.relocation,
      coordinator: route.coordinator,
      // actor-ref carries actor identity only; the target route lives in
      // command 44's route union rather than command 45's actor field.
      actor: { ...route.actor, nodeRid: '' },
      session: route.session,
      action: 'commit' as const,
      result: 'applied' as const,
      currentAuthorityOwnerGeneration: 12n,
      lastAcceptedSessionSequence: 41n
    };
    assert.equal(await dispatchAck(encodeSessionRelocationRouted(routed)), true);
    assert.deepEqual(await firstRoute, routed);
    assert.equal(await dispatchAck(encodeSessionRelocationRouted({
      ...routed,
      result: 'alreadyApplied'
    })), true);
    await assert.rejects(
      dispatchAck(encodeSessionRelocationRouted({ ...routed, result: 'stale' })),
      error => error instanceof Error && /different bytes/.test(error.message)
    );
    const retriedRoute = runtime.requestSessionRelocationRoute(
      'mesh-a', 'session-owner', route
    );
    assert.equal(sent.length, 4);
    const alreadyApplied = { ...routed, result: 'alreadyApplied' as const };
    assert.equal(await dispatchAck(encodeSessionRelocationRouted(alreadyApplied)), true);
    assert.deepEqual(await retriedRoute, alreadyApplied);

    for (const [offset, result] of [
      [1n, 'stale'],
      [2n, 'sessionOrBindingClosed']
    ] as const) {
      const refusedRoute: ServiceSessionRelocationRoute = {
        ...route,
        relocation: { ...route.relocation, low: route.relocation.low + offset }
      };
      const refused = runtime.requestSessionRelocationRoute(
        'mesh-a', 'session-owner', refusedRoute
      );
      const refusal: ServiceSessionRelocationRouted = {
        ...routed,
        relocation: refusedRoute.relocation,
        result,
        currentAuthorityOwnerGeneration: 999n,
        lastAcceptedSessionSequence: 0n
      };
      assert.equal(await dispatchAck(encodeSessionRelocationRouted(refusal)), true);
      assert.deepEqual(await refused, refusal);
    }
  } finally {
    await runtime.dispose();
  }
});

test('target shares an in-flight operation across exact control retries', async () => {
  const envelope = relocationEnvelope();
  const encodedEnvelope = encodeServiceRelocationEnvelope(envelope);
  const request = relocationPrepare(envelope, {
    reference: 'shared-retry-root',
    checksumCrc32c: crc32c(encodedEnvelope)
  });
  const response: ZLinkServiceRelocationControlResponse = {
    kind: 'ready',
    relocation: request.relocation,
    targetAttemptGeneration: request.targetAttemptGeneration,
    round: request.round,
    coordinator: request.coordinator,
    candidate: request.candidate,
    object: request.object,
    role: 'target',
    offeredMessages: request.requiredMessages,
    offeredBytes: request.requiredBytes,
    participants: [],
    sourceNodeGeneration: request.sourceNodeGeneration,
    targetNodeGeneration: request.candidate.nodeGeneration,
    reservationGeneration: request.targetAttemptGeneration,
    root: request.root,
    applicationVersion: request.applicationVersion,
    participantProgress: envelope.participants.map((participant, index) => ({
      participantId: BigInt(index + 1),
      acceptedBoundary: participant.queuedMessages.at(-1)?.sequence ?? participant.replayCursor,
      replayCursor: participant.replayCursor
    }))
  };
  let release!: () => void;
  const held = new Promise<void>(resolve => { release = resolve; });
  let handled = 0;
  let replies = 0;
  const runtime = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => ({
      sendToNode: () => {
        replies += 1;
        return SubmitResult.Ok;
      }
    })
  } as never);
  (runtime as unknown as {
    handleControl: () => Promise<ZLinkServiceRelocationControlResponse>;
  }).handleControl = async () => {
    handled += 1;
    await held;
    return response;
  };
  const payload = encodeServiceRelocationControlRequest(request);
  const parts = [Message.from(payload), Message.from(payload)];
  try {
    const first = runtime.tryHandleControl('mesh-a', {
      sourceNodeRid: 'node-source',
      parts: [parts[0]!]
    } as never);
    const second = runtime.tryHandleControl('mesh-a', {
      sourceNodeRid: 'node-source',
      parts: [parts[1]!]
    } as never);
    assert.equal(handled, 1);
    release();
    assert.deepEqual(await Promise.all([first, second]), [true, true]);
    assert.equal(handled, 1);
    assert.equal(replies, 2);
  } finally {
    parts.forEach(part => part.close());
    await runtime.dispose();
  }
});

test('target reconstructs a striped shared envelope before prepare validation', async () => {
  const envelope = relocationEnvelope();
  const encoded = encodeServiceRelocationEnvelope(envelope);
  const splitAt = Math.floor(encoded.byteLength / 2);
  const stripes = [encoded.subarray(0, splitAt), encoded.subarray(splitAt)];
  const store = new MemoryPublicRelocationStore();
  const stripeRows = await Promise.all(stripes.map(async (bytes, index) => {
    const reference = `stripe-${index}`;
    await store.put({ value: reference }, bytes, 60_000);
    return {
      reference,
      checksumCrc32c: crc32c(bytes),
      byteLength: bytes.byteLength
    };
  }));
  const manifest = Buffer.from(JSON.stringify({
    kind: 'zlink-relocation-striped-v1',
    byteLength: encoded.byteLength,
    checksumCrc32c: crc32c(encoded),
    stripes: stripeRows
  }), 'utf8');
  await store.put({ value: 'striped-root' }, manifest, 60_000);
  const runtime = new ZLinkHostServiceRelocationRuntime({
    relocationStore: () => store
  } as never);
  const restored = await (runtime as unknown as {
    readSharedEnvelope: (
      root: { readonly reference: string; readonly checksumCrc32c: number }
    ) => Promise<ServiceRelocationEnvelope>;
  }).readSharedEnvelope({
    reference: 'striped-root',
    checksumCrc32c: crc32c(encoded)
  });
  assert.ok(encodeServiceRelocationEnvelope(restored).equals(encoded));
});

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
      adoptCreatedAuthority: () => events.push('actor-authority'),
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

test('target opens admission at command 35 pending and converges session routes without command 35 completed', async () => {
  const events: string[] = [];
  const envelope = relocationEnvelope();
  const encodedEnvelope = encodeServiceRelocationEnvelope(envelope);
  const root = { reference: 'shared-root-open', checksumCrc32c: crc32c(encodedEnvelope) };
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
  const authorities = new Map<string, ZLinkAuthoritySnapshot>([
    [spotKey, relocationAuthority('user_spot', 'SpotType')],
    [actorKey, relocationAuthority('actor', 'ActorType')]
  ]);
  const publishAuthorities = (phase: 'sourceCleanupPending' | 'sourceCleanupCompleted') => {
    for (const [key, current] of authorities) {
      authorities.set(key, {
        ...current,
        storeVersion: { value: `committed-${phase}-${key}` } as never,
        authorityOwnerGeneration: 2n,
        ownerId: 'owner-target',
        ownerLeaseGeneration: 8n,
        payload: new ServiceRelocationAuthorityPayloadCodec().publish(
          Buffer.from('authority-state'),
          { ...publication, phase }
        )
      });
    }
  };
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
      reserveRelocationCapacity: async (request: { readonly reservationId: string }) => ({
        kind: 'reserved' as const,
        fence: { value: request.reservationId }
      }),
      abortRelocationCapacity: async () => 'aborted' as const,
      prepareAggregate: async (request: { readonly aggregateId: { readonly value: string };
        readonly aggregateGeneration: bigint }) => ({ kind: 'prepared' as const, fence: {
        aggregateId: request.aggregateId,
        aggregateGeneration: request.aggregateGeneration
      } }),
      commitAggregate: async () => {
        publishAuthorities('sourceCleanupPending');
        return { kind: 'committed' as const };
      },
      abortAggregate: async () => ({ kind: 'aborted' as const })
    }),
    relocationStore: () => ({
      read: async (reference: { readonly value: string }) =>
        reference.value === root.reference
          ? foundBlob(encodedEnvelope)
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
          commitActorJoin: () => undefined };
      },
      publishRelocationSpot: async () => events.push('publish-spot'),
      abortRelocationSpot: async () => events.push('abort-spot'),
      dispatchRoutedActorPacket: async () => undefined
    }),
    actorManager: () => ({
      adoptCreatedAuthority: () => undefined,
      prepareRelocationActor: async () => {
        events.push('prepare-actor');
        return { context: { actorId: 'actor-a' }, configure() {} };
      },
      getState: () => ({
        spotId: 'spot-a',
        setJoinedSpot: () => undefined,
        setLocationGeneration: () => undefined,
        setOwnerLeaseGeneration: () => undefined
      }),
      publishRelocationActor: () => events.push('publish-actor'),
      abortRelocationActor: () => events.push('abort-actor')
    }),
    actorTransfer: {
      publishRoutedActorOwnership: async (actor: { context: { actorId: string } }) => {
        events.push(`cmd44:${actor.context.actorId}`);
      },
      openRoutedActorSession: async (actor: { context: { actorId: string } }) => {
        events.push(`cmd42:${actor.context.actorId}`);
      }
    }
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
  await roundTrip({ ...offered, role: 'source', offeredMessages: 0n,
    offeredBytes: 0n, participants: prepare.participants });
  const control = (phase: 'prepared' | 'committed') => ({ kind: 'data' as const,
    relocation: prepare.relocation, targetAttemptGeneration: prepare.targetAttemptGeneration,
    coordinator: prepare.coordinator, senderRole: 'source' as const, participantId: 1n,
    sequence: 1n, source: sourceFence(), object: prepare.object, phase });
  await roundTrip(control('prepared'));
  await roundTrip(control('committed'));
  await roundTrip({ kind: 'data', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', participantId: 1n, sequence: 1n,
    frozenRecord: acceptedFrozenRecord(envelope, prepare.coordinator, prepare.candidate) });
  await roundTrip({ kind: 'seal', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', response: true, participants: [
      { participantId: 1n, highWater: 1n }, { participantId: 2n, highWater: 0n }
    ] });

  // Command 35 'pending' opens application admission: registry publish and
  // dispatch switch happen now, while sourceCleanupState is still pending.
  const published = await roundTrip({ kind: 'complete', relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source', source: sourceFence(), sourceCleanupState: 'pending' });
  assert.equal(published.kind, 'complete');
  const internals = target as unknown as {
    targetStages: Map<string, { phase: string; converged: boolean }>;
  };
  const stage = [...internals.targetStages.values()][0]!;
  assert.equal(stage.phase, 'open');
  assert.ok(events.includes('publish-spot'));
  assert.ok(events.includes('publish-actor'));

  // The command 44 -> 42 session-route chain converges in the background and
  // the command 42 seal release never precedes the command 44 publication.
  await waitUntil(() => events.includes('cmd42:actor-a'));
  assert.ok(events.indexOf('cmd44:actor-a') < events.indexOf('cmd42:actor-a'));
  assert.equal(stage.converged, true);

  // The stage stays resident until command 35 'completed', so the recovery
  // poller short-circuits instead of restoring the same publication twice.
  const eventCount = events.length;
  await target.recoverPublishedAuthority(authorities.get(spotKey)!);
  assert.equal(events.length, eventCount);
  assert.equal(internals.targetStages.size, 1);

  // Command 35 'completed' only performs relay and durable bookkeeping.
  publishAuthorities('sourceCleanupCompleted');
  const complete = { kind: 'complete' as const, relocation: prepare.relocation,
    targetAttemptGeneration: prepare.targetAttemptGeneration, coordinator: prepare.coordinator,
    senderRole: 'source' as const, source: sourceFence(),
    sourceCleanupState: 'completed' as const };
  const completed = await roundTrip(complete);
  assert.equal(completed.kind, 'complete');
  assert.equal(internals.targetStages.size, 0);
  assert.deepEqual(events.slice(eventCount), []);
  // An exact command 35 retry after completion is answered idempotently.
  assert.deepEqual(await roundTrip(complete), completed);
  // The completed publication is remembered, so recovery stays a no-op.
  await target.recoverPublishedAuthority(authorities.get(spotKey)!);
  assert.deepEqual(events.slice(eventCount), []);
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
      adoptCreatedAuthority: (
        _actorId: string,
        authorityOwnerGeneration: bigint,
        ownerLeaseGeneration: bigint
      ) => {
        void authorityOwnerGeneration;
        void ownerLeaseGeneration;
        targetState.setLocationGeneration();
        targetState.setOwnerLeaseGeneration();
      },
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

test('standalone relocation refuses to freeze a connection-bound accepted send and rolls back the source', async () => {
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
    key: { kind: 'actor', globalId: 'actor-connection-bound' },
    intent: {
      stableType: 'ActorType',
      requestContentReference: 'create:actor-connection-bound',
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 1n
    },
    target: sourcePlacement,
    creatingPayload: Buffer.from('creating'),
    capacity: { actors: 1, spots: 0 }
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  const creationTerminal = Buffer.from('actor-connection-bound-created');
  const activated = await location.completeCreation({
    key: { kind: 'actor', globalId: 'actor-connection-bound' },
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
  const targetActor = { context: { actorId: 'actor-connection-bound' }, configure() {} };
  const targetState = {
    spotId: undefined as string | undefined,
    setJoinedSpot: (spotId: string) => { targetState.spotId = spotId; },
    setLocationGeneration: () => undefined,
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
    locationStore: () => location as never,
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
      adoptCreatedAuthority: () => undefined,
      prepareRelocationActor: async () => targetActor,
      getState: () => targetState,
      publishRelocationActor: () => events.push('target-published'),
      abortRelocationActor: async () => events.push('target-aborted')
    }),
    actorTransfer: {
      publishRoutedActorOwnership: async () => events.push('route-published'),
      openRoutedActorSession: async () => events.push('target-open')
    }
  } as never);
  const sourceActor = { context: { actorId: 'actor-connection-bound' } };
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
          handoffBacklog: [{
            ...productionQueuedPacket(),
            remoteBoundSessionTarget: {
              routerChannelId: 'session.route',
              targetNodeRid: 'session-node',
              spotId: 'session-entry',
              bindingGeneration: '3'
            }
          }],
          setReplayResults: () => undefined,
          commit: async () => events.push('source-committed'),
          rollback: async () => events.push('source-rollback')
        };
      }
    }
  } as never);
  const sourceState = {
    actorId: 'actor-connection-bound',
    actor: sourceActor,
    actorType: 'ActorType',
    meshName: 'mesh-a',
    spotMembershipEpoch: 1n
  };

  await assert.rejects(
    (source as unknown as {
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
    } as never, 7n),
    /connection-bound/
  );

  assert.ok(events.includes('source-sealed'));
  assert.ok(events.includes('source-rollback'));
  assert.equal(events.includes('source-committed'), false);
  const current = await location.readAuthority(
    encodeAuthorityKey('actor', 'actor-connection-bound')
  );
  assert.equal(current.kind, 'snapshot');
  if (current.kind !== 'snapshot') return;
  assert.equal(current.ownerId, 'owner-source');
  assert.equal(current.ownerLeaseGeneration, 7n);
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
      adoptCreatedAuthority: (
        _actorId: string,
        authorityOwnerGeneration: bigint,
        ownerLeaseGeneration: bigint
      ) => {
        void authorityOwnerGeneration;
        void ownerLeaseGeneration;
        targetState.setLocationGeneration();
        targetState.setOwnerLeaseGeneration();
      },
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
    clearJoinedSpot(): void;
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
      adoptCreatedAuthority: (
        actorId: string,
        authorityOwnerGeneration: bigint,
        ownerLeaseGeneration: bigint
      ) => {
        const state = targetActorStates.get(actorId);
        state?.setLocationGeneration(authorityOwnerGeneration);
        state?.setOwnerLeaseGeneration(ownerLeaseGeneration);
      },
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
          clearJoinedSpot() {
            state.spotId = undefined;
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
  const relocatedRoomActor = await location.readAuthority(
    encodeAuthorityKey('actor', 'room-actor')
  );
  assert.equal(relocatedRoomActor.kind, 'snapshot');
  if (relocatedRoomActor.kind === 'snapshot') {
    const identity = decodeActorAuthorityIdentity(relocatedRoomActor.payload);
    assert.equal(identity?.actor.nodeRid, 'node-target');
    assert.equal(identity?.spotId, 'room-a');
    assert.equal(identity?.spotGeneration, 1n);
  }
  assert.ok(events.includes('restore:room-a'));
  assert.ok(events.includes('restore:matchmaker-a'));
  assert.ok(events.includes('restore:room-actor'));
  assert.ok(events.includes('restore:standalone-actor'));
  assert.ok(
    events.indexOf('source-spot-sealed:room-a')
      < events.indexOf('source-session-sealed:room-actor')
  );
  assert.ok(events.includes('membership-restored:room-actor:room-a'));
  assert.ok(events.includes('queue-replayed:room-actor'));
  assert.ok(events.includes('timer-restored:room-a:1'));
  // Session-route convergence (command 44 then 42) runs in a background
  // chain after admission opened and no longer waits for source cleanup.
  await waitUntil(() => ['room-actor', 'standalone-actor'].every(actorId =>
    events.includes(`admission-open:${actorId}`)));
  for (const actorId of ['room-actor', 'standalone-actor']) {
    assert.ok(events.includes(`source-actor-cleanup:${actorId}`));
    // Target admission (registry publish) never waits for source cleanup.
    assert.ok(
      events.indexOf(`published-actor:${actorId}`)
        < events.indexOf(`source-actor-cleanup:${actorId}`)
    );
    // The command 42 seal release stays ordered after the command 44 ACK.
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
    const actorPayload = encodeActorAuthorityIdentity({
      actorType: stableType,
      actor: {
        actorId: globalId,
        objectGeneration: 1n,
        meshName: target.meshName,
        nodeRid: target.nodeRid as never
      },
      meshName: target.meshName,
      ownerNodeGeneration: target.nodeLifecycleGeneration,
      owner: target.owner
    });
    const completed = await store.completeCreation({
      key: { kind, globalId },
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target: target as never,
      completion: {
        kind: 'created',
        // The original authority predates the Actor join. Relocation must derive
        // the target Spot route from the captured aggregate membership.
        readyPayload: actorPayload,
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
