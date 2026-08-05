import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { test } from 'node:test';
import './m6b-execution-policy.contract';
import './m6b-user-spot-terminal-replay.contract';

import { Message, RequestResult, SubmitResult } from '@zlink-systems/zlink';
import {
  ServiceWireCommand,
  ServiceWireFlag
} from '../../../../runtime/protocol/generated/node/service_wire_constants';
import {
  OperationCancelledError,
  OperationRegistry,
  OperationTimeoutError,
  type OperationClock
} from '../../packages/framework/src/runtime/foundation/operation-registry';
import {
  ReadyDomain,
  ReceiveKind,
  type ReceiveRecord
} from '../../packages/framework/src/runtime/foundation/service-runtime-contracts';
import {
  ZLinkNodeRawMeshBackend
} from '../../packages/framework/src/runtime/backend/node/node-raw-mesh-backend';
import type {
  RawServiceIngressRecord,
  RawServiceMeshRuntime
} from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import {
  ServiceInstanceActivationRedirectError,
  ServiceStatefulRuntime,
  type ServiceAsyncInstanceActivationAuthority,
  type ServiceInstanceActivationAuthority
} from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import {
  ServiceStaleGenerationError,
  ServiceStatefulRegistry,
  ServiceTerminalOperationRegistry
} from '../../packages/framework/src/runtime/foundation/service-stateful-registry';
import {
  M6bServiceWireCommand,
  M6bServiceWireFlag,
  decodeStatefulHeader,
  decodeStatefulReply,
  encodeActorCreateHeader,
  encodeActorHeader,
  encodeBoundSessionBindHeader,
  encodeInstanceSpotActivationHeader,
  encodeInstanceSpotHeader,
  encodeMessageFollowHeader,
  encodeSpotHeader,
  encodeStatefulReply,
  encodeUserSpotCloseHeader,
  encodeUserSpotCreateHeader,
  type ServiceInstanceRouteFence
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';
import {
  encodeApplicationPayload
} from '../../packages/framework/src/runtime/foundation/service-wire-m6a-codec';
import { crc32c } from '../../packages/framework/src/runtime/foundation/service-relocation-runtime';
import {
  decodeServiceReadySpotAuthority,
  encodeServiceInstanceAuthorityPayload
} from '../../packages/framework/src/runtime/foundation/service-authority-payload-codec';
import {
  decodeInstanceActivationRecoveryEnvelope,
  encodeInstanceActivationRecoveryEnvelope
} from '../../packages/framework/src/runtime/foundation/service-instance-activation-recovery-codec';
import {
  encodeServiceMetadataFrame,
  validateServiceMetadataFrame
} from '../../packages/framework/src/runtime/foundation/service-metadata-codec';
import {
  ZLinkStatefulAuthorityRouteRuntime
} from '../../packages/framework/src/runtime/host/stateful-authority-route-runtime';
import {
  ZLinkInstanceActivationAuthority
} from '../../packages/framework/src/runtime/host/instance-activation-authority';
import {
  ZLinkInMemoryAuthorityStore
} from '../../packages/framework/src/runtime/locations/in-memory-authority-store';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot
} from '../../packages/framework/src/contracts/Locations/Authority';
import type { ZLinkAuthorityStore } from '../../packages/framework/src/runtime/locations/internal-store-contracts';
import { ZLinkAuthorityScanCursor } from '../../packages/framework/src/contracts/Locations/Authority';
import type { ZLinkBackendMeshNode } from '../../packages/framework/src/runtime/backend/contracts';
import type { ZLinkBackendMessageLike } from '../../packages/framework/src/runtime/backend/runtime-values';
import type {
  ZLinkBlobReference,
  ZLinkRelocationStore
} from '../../packages/framework/src/contracts/Locations/RelocationStore';

import {
  DefaultZLinkSpotManager,
  DefaultZLinkSpotOutbound,
  ZLinkSpotSerialExecutor
} from '../../packages/framework/src/runtime/spots';
import {
  hasObjectClientCapability,
  ZLinkHostSpotAddressTransport
} from '../../packages/framework/src/runtime/host/spot-address-transport';
import {
  encodeChannelEnvelopeParts,
  encodeChannelReplyParts,
  ZLinkChannelMessageKind
} from '../../packages/framework/src/runtime/channels/channel-envelope';
import {
  ZLinkRuntimeRouteTransport
} from '../../packages/framework/src/runtime/channels/channel-transports';
import type {
  ZLinkInstanceSpot,
  ZLinkInstanceSpotContext,
  ZLinkSpotPacketHandler
} from '../../packages/framework/src/contracts';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkMessageMetadataEmpty,
  ZLinkSpotKind
} from '../../packages/framework/src/contracts';
import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind,
  internalFrameworkErrorKind
} from '../../packages/framework/src/runtime/framework-errors-internal';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../../packages/framework/src/runtime/messaging/submission-result';
import { meshActorSessionNodeAdapter } from '../../packages/framework/src/runtime/backend/mesh-actor-session-node-adapter';
import { ZLinkNativeFallbackBoundSession } from '../../packages/framework/src/runtime/streams/native-fallback-bound-session';

test('M6B command and flag constants match the generated service wire schema', () => {
  for (const name of Object.keys(M6bServiceWireCommand) as Array<keyof typeof M6bServiceWireCommand>) {
    assert.equal(M6bServiceWireCommand[name], ServiceWireCommand[name]);
  }
  for (const name of Object.keys(M6bServiceWireFlag) as Array<keyof typeof M6bServiceWireFlag>) {
    assert.equal(M6bServiceWireFlag[name], ServiceWireFlag[name]);
  }
});

test('stale native bound-session binding falls through to the routed session target', () => {
  const adapter = meshActorSessionNodeAdapter({
    sendActorBoundSession: () => SubmitResult.InvalidState
  } as unknown as ZLinkBackendMeshNode);

  assert.deepEqual(
    adapter.sendActorBoundSession(
      { actorId: 'actor-a', nodeRid: 'node-a', generation: 7n },
      3n,
      [],
      0
    ),
    { status: ZLinkSubmitStatus.TargetNotFound }
  );
});

test('unbound native fallback does not attempt a native bound-session send', async () => {
  let nativeSubmits = 0;
  const session = new ZLinkNativeFallbackBoundSession({
    runtime: {
      async submitLocalBoundSession() {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      },
      async sendNativeBoundSession() {
        nativeSubmits += 1;
        return { status: ZLinkSubmitStatus.Submitted };
      },
      async sendBoundSession() {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      }
    } as never,
    routedTransport: {} as never,
    actorRefProvider: () => ({
      actorId: 'actor-unbound',
      objectGeneration: 2n,
      meshName: 'mesh',
      nodeRid: 'node-a',
      bindingGeneration: 0n
    }),
    nativeActorNodeProvider: () => ({}) as never,
    localActorProvider: () => true,
    remoteBoundSessionTargetProvider: () => undefined,
    remoteActorPacketTargetProvider: () => undefined,
    actorId: 'actor-unbound',
    reportError: () => undefined
  });

  class SessionNotice {}
  await assert.rejects(
    () => session.send(new SessionNotice()).submit(),
    error => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.InvalidOperation
  );
  assert.equal(nativeSubmits, 0);
});

test('unbound native fallback disconnect reports session-not-bound without native close', async () => {
  let nativeDisconnects = 0;
  let routedDisconnects = 0;
  const session = new ZLinkNativeFallbackBoundSession({
    runtime: {
      async disconnectNativeBoundSession() {
        nativeDisconnects += 1;
      },
      async disconnectBoundSession() {
        routedDisconnects += 1;
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
          'No current session binding exists for actor \'actor-unbound\'.',
          true
        );
      },
      async submitLocalBoundSession() {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      },
      async sendBoundSession() {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      }
    } as never,
    routedTransport: {} as never,
    actorRefProvider: () => ({
      actorId: 'actor-unbound',
      objectGeneration: 2n,
      meshName: 'mesh',
      nodeRid: 'node-a',
      bindingGeneration: 0n
    }),
    nativeActorNodeProvider: () => ({}) as never,
    localActorProvider: () => true,
    remoteBoundSessionTargetProvider: () => undefined,
    remoteActorPacketTargetProvider: () => undefined,
    actorId: 'actor-unbound',
    reportError: () => undefined
  });

  await assert.rejects(
    () => session.disconnect(),
    error => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.InvalidOperation
  );
  assert.equal(nativeDisconnects, 0);
  assert.equal(routedDisconnects, 1);
});

test('routed bound-session sends use infrastructure node routing after native binding becomes stale', async () => {
  let directSubmits = 0;
  let infrastructureSubmits = 0;
  let spotRoute: unknown;
  const session = new ZLinkNativeFallbackBoundSession({
    runtime: {
      async submitLocalBoundSession() {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      },
      async sendNativeBoundSession() {
        return { status: ZLinkSubmitStatus.TargetNotFound };
      }
    } as never,
    routedTransport: {
      async submit() {
        directSubmits += 1;
        return { status: ZLinkSubmitStatus.TargetNotFound };
      },
      async submitInfrastructure() {
        infrastructureSubmits += 1;
        return { status: ZLinkSubmitStatus.Submitted };
      },
      async sendToSpot(route: unknown) {
        spotRoute = route;
        return { status: ZLinkSubmitStatus.Submitted };
      }
    } as never,
    actorRefProvider: () => ({
      actorId: 'actor-a',
      objectGeneration: 7n,
      meshName: 'mesh',
      nodeRid: 'node-a',
      bindingGeneration: 3n
    }),
    nativeActorNodeProvider: () => ({}) as never,
    localActorProvider: () => true,
    remoteBoundSessionTargetProvider: () => ({
      routerChannelId: 'mesh',
      targetNodeRid: 'gateway',
      spotId: 'gateway'
    }),
    remoteActorPacketTargetProvider: () => undefined,
    actorId: 'actor-a',
    reportError: () => undefined
  });

  class SessionNotice {}
  await session.send(new SessionNotice()).submit();
  assert.equal(directSubmits, 0);
  assert.equal(infrastructureSubmits, 1);
  assert.equal(spotRoute, undefined);
});

test('Message Follow command preserves route fences and rejects mismatched objects', () => {
  const source = {
    kind: 'actor' as const,
    actor: { actorId: 'actor-follow', nodeRid: '', generation: 7n },
    targetNodeRid: 'node-old',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 13n,
    ownerLeaseGeneration: 17n
  };
  const target = {
    ...source,
    actor: { ...source.actor, nodeRid: '' },
    targetNodeRid: 'node-new',
    targetNodeGeneration: 19n,
    authorityOwnerGeneration: 23n,
    ownerLeaseGeneration: 29n
  };
  const encoded = encodeMessageFollowHeader({
    source,
    target,
    hopCount: 2,
    queuedMessages: 3,
    queuedBytes: 4096,
    originalOperation: { high: 31n, low: 37n },
    originalReplyRouteId: 41n
  });
  assert.deepEqual(decodeStatefulHeader(encoded), {
    kind: 'messageFollow',
    source,
    target,
    hopCount: 2,
    queuedMessages: 3,
    queuedBytes: 4096,
    originalOperation: { high: 31n, low: 37n },
    originalReplyRouteId: 41n
  });
  assert.throws(() => encodeMessageFollowHeader({
    source,
    target: { ...target, actor: { ...target.actor, actorId: 'other' } },
    hopCount: 2,
    queuedMessages: 3,
    queuedBytes: 4096,
    originalOperation: { high: 31n, low: 37n },
    originalReplyRouteId: 41n
  }), /identities differ/);
});

test('Message Follow invalidates a Spot route only when every source fence still matches', () => {
  let ingress: ((record: import('../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime')
    .RawServiceIngressRecord) => unknown) | undefined;
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'node-old'
        ? { descriptor: { lifecycleGeneration: 11n } }
        : nodeRid === 'node-new'
          ? { descriptor: { lifecycleGeneration: 19n } }
          : undefined
    },
    setServiceIngress(handler: typeof ingress) { ingress = handler; },
    sendService: () => true
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'caller', 5n);
  const source = {
    kind: 'spot' as const,
    spot: { spotId: 'room', generation: 7n },
    targetNodeRid: 'node-old',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 13n,
    ownerLeaseGeneration: 17n
  };
  const target = {
    ...source,
    targetNodeRid: 'node-new',
    targetNodeGeneration: 19n,
    authorityOwnerGeneration: 23n,
    ownerLeaseGeneration: 29n
  };
  runtime.rememberSpotRoute({ ...source, storeVersion: 'source-v1' });
  const stale = encodeMessageFollowHeader({
    source: { ...source, authorityOwnerGeneration: 12n },
    target,
    hopCount: 1,
    queuedMessages: 1,
    queuedBytes: 128,
    originalOperation: { high: 1n, low: 2n },
    originalReplyRouteId: 0n
  });
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.messageFollow,
    flags: 0,
    sourceRoutingId: 'node-old',
    parts: [stale]
  }), 'infrastructure');
  const spotRoutes = (runtime as unknown as {
    readonly spotRoutes: Map<string, unknown>;
  }).spotRoutes;
  assert.equal(spotRoutes.size, 1);

  const exact = encodeMessageFollowHeader({
    source,
    target,
    hopCount: 1,
    queuedMessages: 1,
    queuedBytes: 128,
    originalOperation: { high: 1n, low: 2n },
    originalReplyRouteId: 0n
  });
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.messageFollow,
    flags: 0,
    sourceRoutingId: 'node-old',
    parts: [exact]
  }), 'infrastructure');
  assert.equal(spotRoutes.size, 0);
  runtime.close();
});

test('Actor Message Follow reaches the owner cache invalidator only from the admitted source', () => {
  let ingress: ((record: import('../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime')
    .RawServiceIngressRecord) => unknown) | undefined;
  const received: unknown[] = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'node-old'
        ? { descriptor: { lifecycleGeneration: 11n } }
        : nodeRid === 'node-new'
          ? { descriptor: { lifecycleGeneration: 23n } }
          : undefined
    },
    setServiceIngress(handler: typeof ingress) { ingress = handler; },
    sendService: () => true
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'caller', 5n);
  runtime.setMessageFollowHandler(record => received.push(record));
  const source = {
    kind: 'actor' as const,
    actor: { actorId: 'actor-follow', nodeRid: 'node-old', generation: 7n },
    targetNodeRid: 'node-old',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 13n,
    ownerLeaseGeneration: 17n
  };
  const target = {
    kind: 'actor' as const,
    actor: { actorId: 'actor-follow', nodeRid: 'node-new', generation: 7n },
    targetNodeRid: 'node-new',
    targetNodeGeneration: 23n,
    authorityOwnerGeneration: 29n,
    ownerLeaseGeneration: 31n
  };
  const encoded = encodeMessageFollowHeader({
    source,
    target,
    hopCount: 1,
    queuedMessages: 1,
    queuedBytes: 128,
    originalOperation: { high: 37n, low: 41n },
    originalReplyRouteId: 43n
  });

  assert.equal(ingress?.({
    command: M6bServiceWireCommand.messageFollow,
    flags: 0,
    sourceRoutingId: 'different-node',
    parts: [encoded]
  }), 'protocolError');
  assert.equal(received.length, 0);
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.messageFollow,
    flags: 0,
    sourceRoutingId: 'node-old',
    parts: [encoded]
  }), 'infrastructure');
  assert.deepEqual(received, [{
    kind: 'messageFollow',
    source: { ...source, actor: { ...source.actor, nodeRid: '' } },
    target: { ...target, actor: { ...target.actor, nodeRid: '' } },
    hopCount: 1,
    queuedMessages: 1,
    queuedBytes: 128,
    originalOperation: { high: 37n, low: 41n },
    originalReplyRouteId: 43n
  }]);
  const nonIncreasing = encodeMessageFollowHeader({
    source,
    target: { ...target, authorityOwnerGeneration: source.authorityOwnerGeneration },
    hopCount: 1,
    queuedMessages: 1,
    queuedBytes: 128,
    originalOperation: { high: 37n, low: 42n },
    originalReplyRouteId: 43n
  });
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.messageFollow,
    flags: 0,
    sourceRoutingId: 'node-old',
    parts: [nonIncreasing]
  }), 'protocolError');
  assert.equal(received.length, 1);
  const staleTarget = encodeMessageFollowHeader({
    source,
    target: { ...target, targetNodeGeneration: 24n },
    hopCount: 1,
    queuedMessages: 1,
    queuedBytes: 128,
    originalOperation: { high: 37n, low: 42n },
    originalReplyRouteId: 43n
  });
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.messageFollow,
    flags: 0,
    sourceRoutingId: 'node-old',
    parts: [staleTarget]
  }), 'protocolError');
  assert.equal(received.length, 1);
  runtime.close();
});

test('remote User Spot create and close records preserve every generation fence exactly', () => {
  const create = encodeUserSpotCreateHeader({
    correlation: 71n,
    operation: { high: 5n, low: 9n },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 11n,
    spotId: 'spot-1',
    stableType: 'Room',
    reservation: {
      reservationId: 'reservation-1',
      expectedStoreVersion: 'version-1',
      objectGeneration: 13n,
      authorityOwnerGeneration: 17n,
      targetNodeRid: 'target',
      targetNodeGeneration: 19n,
      targetOwnerId: 'owner-1',
      targetOwnerLeaseGeneration: 23n,
      pendingCapacityDelta: 1
    },
    deadlineUnixMs: 29n
  });
  assert.deepEqual(decodeStatefulHeader(create), {
    kind: 'userSpotCreate',
    correlation: 71n,
    operation: { high: 5n, low: 9n },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 11n,
    spotId: 'spot-1',
    stableType: 'Room',
    reservation: {
      reservationId: 'reservation-1',
      expectedStoreVersion: 'version-1',
      objectGeneration: 13n,
      authorityOwnerGeneration: 17n,
      targetNodeRid: 'target',
      targetNodeGeneration: 19n,
      targetOwnerId: 'owner-1',
      targetOwnerLeaseGeneration: 23n,
      pendingCapacityDelta: 1
    },
    deadlineUnixMs: 29n
  });

  const close = encodeUserSpotCloseHeader({
    correlation: 73n,
    operation: { high: 31n, low: 37n },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 41n,
    target: {
      spotId: 'spot-1',
      objectGeneration: 43n,
      targetNodeRid: 'target',
      targetNodeGeneration: 47n,
      authorityOwnerGeneration: 53n,
      expectedStoreVersion: 'version-2'
    },
    deadlineUnixMs: 59n
  });
  assert.deepEqual(decodeStatefulHeader(close), {
    kind: 'userSpotClose',
    correlation: 73n,
    operation: { high: 31n, low: 37n },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 41n,
    target: {
      spotId: 'spot-1',
      objectGeneration: 43n,
      targetNodeRid: 'target',
      targetNodeGeneration: 47n,
      authorityOwnerGeneration: 53n,
      expectedStoreVersion: 'version-2'
    },
    deadlineUnixMs: 59n
  });
  assert.throws(() => decodeStatefulHeader(Buffer.concat([create, Buffer.of(0)])));
  assert.throws(() => decodeStatefulHeader(close.subarray(0, -1)));
});

test('remote User Spot command 20 success tails use operation discriminators 13 and 14', () => {
  assert.deepEqual(
    decodeStatefulReply(
      encodeStatefulReply(79n, RequestResult.Ok, 0, {
        kind: 'userSpotCreate',
        createResult: 'created',
        spotId: 'spot-2',
        objectGeneration: 83n
      }),
      79n,
      'userSpotCreate'
    ).tail,
    {
      kind: 'userSpotCreate',
      createResult: 'created',
      spotId: 'spot-2',
      objectGeneration: 83n
    }
  );
  assert.deepEqual(
    decodeStatefulReply(
      encodeStatefulReply(89n, RequestResult.Ok, 0, {
        kind: 'userSpotClose',
        closed: true
      }),
      89n,
      'userSpotClose'
    ).tail,
    { kind: 'userSpotClose', closed: true }
  );
});

test('remote Actor command 49 preserves reservation fences and replays one terminal', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly requestSequence: bigint;
    readonly parts: readonly Buffer[];
  }) => unknown;
  const replies: Array<readonly Buffer[]> = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 5n } }
        : undefined
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    },
    replyService: (_record: unknown, parts: readonly Buffer[]) => {
      replies.push(parts);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 7n);
  let executions = 0;
  runtime.registerUserSpotOperationHandler({
    create: async () => { throw new Error('not used'); },
    close: async () => { throw new Error('not used'); },
    createActor: async record => {
      executions++;
      return {
        terminalResult: RequestResult.Ok,
        failureCode: 0,
        tail: {
          kind: 'actorCreate',
          createResult: 'created',
          actor: {
            nodeRid: 'target',
            actorId: record.actorId,
            generation: record.reservation.objectGeneration
          }
        }
      };
    }
  });
  const request = {
    operation: { high: 11n, low: 13n },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 5n,
    actorId: 'actor-command-49',
    stableType: 'Player',
    reservation: {
      reservationId: 'actor-reservation',
      expectedStoreVersion: 'actor-version',
      objectGeneration: 17n,
      authorityOwnerGeneration: 19n,
      targetNodeRid: 'target',
      targetNodeGeneration: 7n,
      targetOwnerId: 'owner',
      targetOwnerLeaseGeneration: 23n,
      pendingCapacityDelta: 1
    },
    deadlineUnixMs: BigInt(Date.now() + 5_000)
  };
  for (const [index, correlation] of [29n, 31n].entries()) {
    const header = encodeActorCreateHeader({ ...request, correlation });
    const decoded = decodeStatefulHeader(header);
    assert.equal(decoded.kind, 'actorCreate');
    assert.deepEqual(
      decoded.kind === 'actorCreate' ? decoded.reservation : undefined,
      request.reservation
    );
    assert.equal(ingress({
      command: M6bServiceWireCommand.actorCreate,
      flags: 0,
      sourceRoutingId: 'source',
      requestSequence: BigInt(index + 1),
      parts: [header]
    }), 'infrastructure');
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.equal(executions, 1);
  assert.equal(replies.length, 2);
  for (const [index, correlation] of [29n, 31n].entries()) {
    assert.deepEqual(
      decodeStatefulReply(replies[index]![0]!, correlation, 'actorCreate').tail,
      {
        kind: 'actorCreate',
        createResult: 'created',
        actor: {
          nodeRid: 'target',
          actorId: 'actor-command-49',
          generation: 17n
        }
      }
    );
  }
  runtime.close();
});

test('remote User Spot target executes once and rewrites correlation on terminal replay', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly requestSequence: bigint;
    readonly parts: readonly Buffer[];
  }) => unknown;
  const replies: Array<readonly Buffer[]> = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 5n } }
        : undefined
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    },
    replyService: (_record: unknown, parts: readonly Buffer[]) => {
      replies.push(parts);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 7n);
  let executions = 0;
  runtime.registerUserSpotOperationHandler({
    create: async record => {
      executions++;
      return {
        terminalResult: RequestResult.Ok,
        failureCode: 0,
        tail: {
          kind: 'userSpotCreate',
          createResult: 'created',
          spotId: record.spotId,
          objectGeneration: record.reservation.objectGeneration
        }
      };
    },
    close: async () => {
      throw new Error('not used');
    }
  });
  const request = {
    operation: { high: 11n, low: 13n },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 5n,
    spotId: 'spot-replay',
    stableType: 'Room',
    reservation: {
      reservationId: 'reservation',
      expectedStoreVersion: 'version',
      objectGeneration: 17n,
      authorityOwnerGeneration: 19n,
      targetNodeRid: 'target',
      targetNodeGeneration: 7n,
      targetOwnerId: 'owner',
      targetOwnerLeaseGeneration: 23n,
      pendingCapacityDelta: 1
    },
    deadlineUnixMs: BigInt(Date.now() + 250)
  };
  for (const [index, correlation] of [29n, 31n].entries()) {
    const header = encodeUserSpotCreateHeader({ ...request, correlation });
    assert.equal(ingress({
      command: M6bServiceWireCommand.userSpotCreate,
      flags: 0,
      sourceRoutingId: 'source',
      requestSequence: BigInt(index + 1),
      parts: [header]
    }), 'infrastructure');
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.equal(executions, 1);
  assert.equal(replies.length, 2);
  assert.equal(
    decodeStatefulReply(replies[0]![0]!, 29n, 'userSpotCreate').tail?.kind,
    'userSpotCreate'
  );
  assert.equal(
    decodeStatefulReply(replies[1]![0]!, 31n, 'userSpotCreate').tail?.kind,
    'userSpotCreate'
  );
  await new Promise(resolve => setTimeout(resolve, 300));
  const expiredReplay = encodeUserSpotCreateHeader({
    ...request,
    correlation: 33n
  });
  assert.equal(ingress({
    command: M6bServiceWireCommand.userSpotCreate,
    flags: 0,
    sourceRoutingId: 'source',
    requestSequence: 3n,
    parts: [expiredReplay]
  }), 'infrastructure');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(executions, 1);
  assert.equal(
    decodeStatefulReply(replies[2]![0]!, 33n, 'userSpotCreate').tail?.kind,
    'userSpotCreate'
  );
  const expiredNew = encodeUserSpotCreateHeader({
    ...request,
    correlation: 35n,
    operation: { high: 11n, low: 39n },
    deadlineUnixMs: BigInt(Date.now() - 1)
  });
  assert.equal(ingress({
    command: M6bServiceWireCommand.userSpotCreate,
    flags: 0,
    sourceRoutingId: 'source',
    requestSequence: 4n,
    parts: [expiredNew]
  }), 'infrastructure');
  assert.equal(executions, 1);
  assert.equal(
    decodeStatefulReply(replies[3]![0]!, 35n, 'userSpotCreate').terminalResult,
    RequestResult.TimedOut
  );
  const wrongTargetLifecycle = encodeUserSpotCreateHeader({
    ...request,
    correlation: 36n,
    operation: { high: 11n, low: 40n },
    reservation: {
      ...request.reservation,
      targetNodeGeneration: 8n
    },
    deadlineUnixMs: BigInt(Date.now() + 10_000)
  });
  assert.equal(ingress({
    command: M6bServiceWireCommand.userSpotCreate,
    flags: 0,
    sourceRoutingId: 'source',
    requestSequence: 5n,
    parts: [wrongTargetLifecycle]
  }), 'infrastructure');
  assert.equal(executions, 1);
  assert.equal(
    decodeStatefulReply(replies[4]![0]!, 36n, 'userSpotCreate').failureCode,
    34
  );
  const terminalTable = (
    runtime as unknown as {
      admittedUserSpotOperations: Map<string, {
        readonly request: string;
        readonly deadlineUnixMs: bigint;
        readonly result: Promise<unknown>;
        settled: boolean;
      }>;
    }
  ).admittedUserSpotOperations;
  for (let index = terminalTable.size; index < 65_536; index++) {
    terminalTable.set(`occupied-${index}`, {
      request: 'occupied',
      deadlineUnixMs: BigInt(Date.now() + 10_000),
      result: new Promise(() => undefined),
      settled: false
    });
  }
  const overflow = encodeUserSpotCreateHeader({
    ...request,
    correlation: 37n,
    operation: { high: 11n, low: 41n },
    deadlineUnixMs: BigInt(Date.now() + 10_000)
  });
  assert.equal(ingress({
    command: M6bServiceWireCommand.userSpotCreate,
    flags: 0,
    sourceRoutingId: 'source',
    requestSequence: 6n,
    parts: [overflow]
  }), 'infrastructure');
  assert.equal(executions, 1);
  assert.equal(
    decodeStatefulReply(replies[5]![0]!, 37n, 'userSpotCreate').terminalResult,
    RequestResult.Busy
  );
  const originalDateNow = Date.now;
  Date.now = () => originalDateNow() + 5 * 60_000 + 1_000;
  try {
    const retiredReplay = encodeUserSpotCreateHeader({
      ...request,
      correlation: 43n
    });
    assert.equal(ingress({
      command: M6bServiceWireCommand.userSpotCreate,
      flags: 0,
      sourceRoutingId: 'source',
      requestSequence: 7n,
      parts: [retiredReplay]
    }), 'infrastructure');
    assert.equal(executions, 1);
    assert.equal(
      decodeStatefulReply(replies[6]![0]!, 43n, 'userSpotCreate').terminalResult,
      RequestResult.TimedOut
    );
  } finally {
    Date.now = originalDateNow;
  }
  runtime.close();
});

test('remote User Spot command 48 close replays one terminal without closing twice', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly requestSequence: bigint;
    readonly parts: readonly Buffer[];
  }) => unknown;
  const replies: Array<readonly Buffer[]> = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 5n } }
        : undefined
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    },
    replyService: (_record: unknown, parts: readonly Buffer[]) => {
      replies.push(parts);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 7n);
  let executions = 0;
  runtime.registerUserSpotOperationHandler({
    create: async () => {
      throw new Error('not used');
    },
    close: async () => {
      executions++;
      return {
        terminalResult: RequestResult.Ok,
        failureCode: 0,
        tail: { kind: 'userSpotClose', closed: true }
      };
    }
  });
  const request = {
    operation: { high: 41n, low: 43n },
    sourceNodeRid: 'source',
    sourceNodeGeneration: 5n,
    target: {
      spotId: 'spot-close-replay',
      objectGeneration: 17n,
      targetNodeRid: 'target',
      targetNodeGeneration: 7n,
      authorityOwnerGeneration: 19n,
      expectedStoreVersion: 'version-close'
    },
    deadlineUnixMs: BigInt(Date.now() + 5_000)
  };

  for (const [index, correlation] of [47n, 53n].entries()) {
    assert.equal(ingress({
      command: M6bServiceWireCommand.userSpotClose,
      flags: 0,
      sourceRoutingId: 'source',
      requestSequence: BigInt(index + 1),
      parts: [encodeUserSpotCloseHeader({ ...request, correlation })]
    }), 'infrastructure');
    await new Promise(resolve => setImmediate(resolve));
  }

  assert.equal(executions, 1);
  assert.equal(replies.length, 2);
  assert.deepEqual(
    decodeStatefulReply(replies[0]![0]!, 47n, 'userSpotClose').tail,
    { kind: 'userSpotClose', closed: true }
  );
  assert.deepEqual(
    decodeStatefulReply(replies[1]![0]!, 53n, 'userSpotClose').tail,
    { kind: 'userSpotClose', closed: true }
  );
  runtime.close();
});

test('authority keys share the Spot discriminator and preserve colon identities canonically', () => {
  assert.equal(
    encodeAuthorityKey('instance_spot', 'tenant:42').value,
    'zla1:s:9:tenant%3A42'
  );
  assert.equal(
    encodeAuthorityKey('user_spot', 'tenant:42').value,
    'zla1:s:9:tenant%3A42'
  );
});

test('authority payload bytes match the schema fixture shape and reject malformed UTF-8', () => {
  const base = {
    stableType: 'TenantWorker',
    spotId: 'tenant:42',
    ownerId: 'owner-a',
    ownerLeaseGeneration: 5n,
    ownerMeshName: 'mesh-a',
    ownerNodeRid: 'node-a',
    ownerNodeGeneration: 1n
  };
  const cold = encodeServiceInstanceAuthorityPayload({
    ...base,
    state: 'coldActivating'
  });
  const ready = encodeServiceInstanceAuthorityPayload({ ...base, state: 'ready' });
  const activationRecovery = {
    reference: 'relocation:first-message',
    sha256: Buffer.alloc(32, 0x5a),
    encodedSize: 256,
    inboxSequence: 1n,
    replayCursor: 0n
  };
  const recoveringReady = encodeServiceInstanceAuthorityPayload({
    ...base,
    state: 'ready',
    activationRecovery
  });
  assert.deepEqual(
    decodeServiceReadySpotAuthority(recoveringReady)?.activationRecovery,
    activationRecovery
  );
  assert.throws(
    () => encodeServiceInstanceAuthorityPayload({
      ...base,
      state: 'coldActivating',
      activationRecovery
    }),
    /Ready Instance Spot/
  );
  assert.equal(
    cold.toString('hex'),
    '5a4c4155010000000000510102001d03001a0100170c54656e616e74576f726b6572'
      + '0974656e616e743a3432076f776e65722d610000000000000005066d6573682d6106'
      + '6e6f64652d61000000000000000100000000000000000000cfdf6035'
  );
  assert.equal(
    ready.toString('hex'),
    '5a4c4155010000000000510002001d03001a0200170c54656e616e74576f726b6572'
      + '0974656e616e743a3432076f776e65722d610000000000000005066d6573682d6106'
      + '6e6f64652d610000000000000001000000000000000000006761e989'
  );

  const malformed = Buffer.from(ready);
  const spotIdOffset = malformed.indexOf(Buffer.from('tenant:42'));
  assert.notEqual(spotIdOffset, -1);
  malformed[spotIdOffset] = 0xff;
  malformed.writeUInt32BE(crc32c(malformed.subarray(0, -4)), malformed.byteLength - 4);
  assert.equal(decodeServiceReadySpotAuthority(malformed), undefined);

  const closeWithRecovery = Buffer.from(recoveringReady);
  closeWithRecovery[11] = 3;
  closeWithRecovery.writeUInt32BE(
    crc32c(closeWithRecovery.subarray(0, -4)),
    closeWithRecovery.byteLength - 4
  );
  assert.equal(decodeServiceReadySpotAuthority(closeWithRecovery), undefined);

  const actorWithRecovery = Buffer.from(recoveringReady);
  actorWithRecovery[12] = 1;
  actorWithRecovery.writeUInt32BE(
    crc32c(actorWithRecovery.subarray(0, -4)),
    actorWithRecovery.byteLength - 4
  );
  assert.equal(decodeServiceReadySpotAuthority(actorWithRecovery), undefined);

  const userSpotWithRecovery = Buffer.from(recoveringReady);
  userSpotWithRecovery[15] = 2;
  userSpotWithRecovery.writeUInt32BE(
    crc32c(userSpotWithRecovery.subarray(0, -4)),
    userSpotWithRecovery.byteLength - 4
  );
  assert.equal(decodeServiceReadySpotAuthority(userSpotWithRecovery), undefined);

  const recoveryReferenceOffset = recoveringReady.indexOf(Buffer.from(activationRecovery.reference));
  assert.notEqual(recoveryReferenceOffset, -1);
  const activationUnionOffset = recoveryReferenceOffset - 7;
  const relocationUnionOffset = activationUnionOffset - 5;
  const relocationWithRecovery = Buffer.from(recoveringReady);
  relocationWithRecovery[relocationUnionOffset] = 1;
  relocationWithRecovery.writeUInt32BE(
    crc32c(relocationWithRecovery.subarray(0, -4)),
    relocationWithRecovery.byteLength - 4
  );
  assert.equal(decodeServiceReadySpotAuthority(relocationWithRecovery), undefined);
});

test('Instance activation recovery envelope preserves the complete first operation', () => {
  const input = {
    target: {
      targetSpotId: 'tenant:42',
      stableType: 'TenantWorker',
      targetNodeRid: 'node-b',
      targetNodeGeneration: 7n,
      descriptorVersion: 'descriptor-v3'
    },
    targetMeshName: 'mesh-b',
    sourceNodeRid: 'node-a',
    sourceNodeGeneration: 5n,
    sourceSpotId: 'source:spot',
    operationKind: 'request' as const,
    operation: { high: 5n, low: 19n },
    replyRouteId: 23n,
    deadlineUnixMs: 99_999n,
    applicationPayloadFrame: encodeApplicationPayload({
      packetName: 'TenantRequest',
      contentType: 'application/octet-stream',
      payload: Buffer.from('first')
    })
  };
  const encoded = encodeInstanceActivationRecoveryEnvelope(input);
  assert.deepEqual(decodeInstanceActivationRecoveryEnvelope(encoded), input);
  const corrupted = Buffer.from(encoded);
  corrupted[corrupted.byteLength - 1] ^= 0xff;
  assert.throws(
    () => decodeInstanceActivationRecoveryEnvelope(corrupted),
    /checksum/
  );
});

test('Instance activation recovery envelope matches the cross-language golden bytes', () => {
  const encoded = encodeInstanceActivationRecoveryEnvelope({
    target: {
      targetSpotId: 'spot-1',
      stableType: 'quest',
      targetNodeRid: 'target',
      targetNodeGeneration: 7n,
      descriptorVersion: 'descriptor-9'
    },
    targetMeshName: 'main',
    sourceNodeRid: 'source',
    sourceNodeGeneration: 3n,
    sourceSpotId: 'entry',
    operationKind: 'request',
    operation: { high: 0n, low: 9n },
    replyRouteId: 11n,
    deadlineUnixMs: 1_700_000_000_000n,
    metadataFrame: encodeServiceMetadataFrame(new Map([['trace', 'abc']])),
    applicationPayloadFrame: encodeApplicationPayload({
      packetName: 'quest.start',
      contentType: 'application/json',
      payload: Buffer.from('{"x":1}')
    })
  });
  assert.equal(
    encoded.toString('hex'),
    '5a4c4941010000000000a00673706f742d31057175657374046d61696e067461726765'
      + '74'
      + '00000000000000070c64657363726970746f722d3906736f7572636500000000000000'
      + '030105656e7472790200000000000000000000000000000009000000000000000b0000'
      + '018bcfe56800010101057472616365000361626301000000280b71756573742e73746172'
      + '74106170706c69636174696f6e2f6a736f6e000000077b2278223a317de138c97b'
  );
});

test('Spot and Actor wire records preserve identity and reject malformed records', () => {
  const spot = {
    spotId: 'spot-a',
    generation: 7n
  };
  const spotHeader = encodeSpotHeader('spotRequest', 'source', {
    spot,
    targetNodeRid: 'node-b',
    targetNodeGeneration: 3n,
    authorityOwnerGeneration: 9n,
    ownerLeaseGeneration: 4n,
    storeVersion: 'store-v1'
  }, 11n);
  assert.deepEqual(decodeStatefulHeader(spotHeader), {
    kind: 'spotRequest',
    correlation: 11n,
    sourceSpotId: 'source',
    target: {
      spot,
      targetNodeRid: 'node-b',
      targetNodeGeneration: 3n,
      authorityOwnerGeneration: 9n,
      ownerLeaseGeneration: 4n,
      storeVersion: 'store-v1'
    }
  });

  const target = { nodeRid: 'node-b', actorId: 'actor-a', generation: 5n };
  const source = { nodeRid: 'node-a', actorId: 'actor-source', generation: 2n };
  const actorHeader = encodeActorHeader(
    'actorRequest',
    {
      actor: target,
      targetNodeGeneration: 3n,
      authorityOwnerGeneration: 5n
    },
    12n,
    source,
    { sessionRid: 'session-a', bindingGeneration: 4n, sequence: 8n }
  );
  assert.deepEqual(decodeStatefulHeader(actorHeader), {
    kind: 'actorRequest',
    correlation: 12n,
    sourceActor: { nodeRid: '', actorId: source.actorId, generation: source.generation },
    target: {
      actor: target,
      targetNodeGeneration: 3n,
      authorityOwnerGeneration: 5n
    },
    boundSession: {
      sessionRid: 'session-a',
      bindingGeneration: 4n,
      sequence: 8n
    }
  });
  assert.throws(() => decodeStatefulHeader(actorHeader.subarray(0, -1)));
  assert.throws(() => decodeStatefulHeader(Buffer.concat([spotHeader, Buffer.of(0)])));
});

test('stateful replies preserve operation-specific tails', () => {
  const actor = { nodeRid: 'node-b', actorId: 'actor-a', generation: 5n };
  const encoded = encodeStatefulReply(17n, RequestResult.Ok, 0, {
    kind: 'actorLookup',
    actor,
    spot: { spotId: 'spot-a', generation: 7n },
    membershipEpoch: 4n,
    authorityOwnerGeneration: 8n
  });
  assert.deepEqual(decodeStatefulReply(encoded, 17n, 'actorLookup'), {
    correlation: 17n,
    terminalResult: RequestResult.Ok,
    failureCode: 0,
    tail: {
      kind: 'actorLookup',
      actor: { nodeRid: '', actorId: 'actor-a', generation: 5n },
      spot: { spotId: 'spot-a', generation: 7n },
      membershipEpoch: 4n,
      authorityOwnerGeneration: 8n
    }
  });
  assert.throws(() => decodeStatefulReply(encoded, 18n, 'actorLookup'));
});

test('stateful reply decoder rejects a non-canonical terminal and failure pair', () => {
  const malformedPairs = [
    [RequestResult.NotFound, 0],
    [RequestResult.TimedOut, 21],
    [RequestResult.NotConnected, 21],
    [RequestResult.Conflict, 14],
    [RequestResult.InternalError, 14]
  ] as const;
  for (const [terminalResult, failureCode] of malformedPairs) {
    const malformed = encodeStatefulReply(19n, terminalResult, failureCode);
    assert.throws(() => decodeStatefulReply(malformed, 19n, 'instanceSpotRequest'));
  }
  const validBoundary = encodeStatefulReply(20n, RequestResult.TimedOut, 0);
  assert.equal(
    decodeStatefulReply(validBoundary, 20n, 'instanceSpotRequest').terminalResult,
    RequestResult.TimedOut
  );
  const validFailure = encodeStatefulReply(21n, RequestResult.NotFound, 21);
  assert.throws(() => decodeStatefulReply(validFailure, 21n, 'instanceSpotRequest', true));
  const validSuccess = encodeStatefulReply(22n, RequestResult.Ok, 0);
  assert.equal(
    decodeStatefulReply(validSuccess, 22n, 'instanceSpotRequest', true).terminalResult,
    RequestResult.Ok
  );
});

test('outbound stateful routes use resolved authority generations and never object generations', () => {
  const sent: Array<{ readonly target: string; readonly parts: readonly Buffer[] }> = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'node-b' || nodeRid === 'node-c'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    setServiceIngress: () => {},
    sendService: (target: string, parts: readonly Buffer[]) => {
      sent.push({ target, parts });
      return true;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-a', 3n);
  const payload = {
    packetName: 'AuthorityFence',
    contentType: 'application/octet-stream',
    payload: Buffer.from('payload')
  };
  const actor = { nodeRid: 'node-b', actorId: 'actor-a', generation: 5n };
  assert.equal(runtime.sendToActor(actor, 7n, actor.generation, payload), SubmitResult.NotFound);
  runtime.rememberActorRoute({
    actor,
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 11n
  });
  assert.equal(runtime.sendToActor(actor, 7n, actor.generation, payload), SubmitResult.Ok);
  const actorHeader = decodeStatefulHeader(sent.at(-1)!.parts[0]!);
  assert.equal(actorHeader.kind, 'actorSend');
  if (actorHeader.kind === 'actorSend') {
    assert.equal(actorHeader.target.authorityOwnerGeneration, 11n);
  }

  const spot = { spotId: 'spot-a', generation: 6n };
  const firstRoute = {
    spot,
    targetNodeRid: 'node-b',
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 13n,
    ownerLeaseGeneration: 17n,
    storeVersion: 'store-v1'
  };
  assert.equal(runtime.sendToSpot('source', {
    ...firstRoute,
    targetNodeGeneration: 8n
  }, payload), SubmitResult.NotFound);
  assert.equal(runtime.sendToSpot('source', firstRoute, payload), SubmitResult.Ok);
  const firstSpotHeader = decodeStatefulHeader(sent.at(-1)!.parts[0]!);
  assert.equal(firstSpotHeader.kind, 'spotSend');
  if (firstSpotHeader.kind === 'spotSend') {
    assert.deepEqual(firstSpotHeader.target, firstRoute);
  }
  const advancedRoute = { ...firstRoute, storeVersion: 'store-v2' };
  runtime.rememberSpotRoute(advancedRoute);
  assert.equal(runtime.sendToSpot('source', advancedRoute, payload), SubmitResult.Ok);
  const advancedSpotHeader = decodeStatefulHeader(sent.at(-1)!.parts[0]!);
  assert.equal(advancedSpotHeader.kind, 'spotSend');
  if (advancedSpotHeader.kind === 'spotSend') {
    assert.equal(advancedSpotHeader.target.storeVersion, 'store-v2');
  }
  const reactivatedRoute = {
    ...advancedRoute,
    spot: { spotId: spot.spotId, generation: 9n },
    storeVersion: 'store-v3'
  };
  runtime.rememberSpotRoute(reactivatedRoute);
  // Direct application routing uses the logical Spot ID. A caller holding
  // the previous object generation still reaches the current Ready route.
  assert.equal(runtime.sendToSpot('source', firstRoute, payload), SubmitResult.Ok);
  const reactivatedHeader = decodeStatefulHeader(sent.at(-1)!.parts[0]!);
  assert.equal(reactivatedHeader.kind, 'spotSend');
  if (reactivatedHeader.kind === 'spotSend') {
    assert.deepEqual(reactivatedHeader.target, reactivatedRoute);
  }
  const successorRoute = {
    spot: reactivatedRoute.spot,
    targetNodeRid: 'node-c',
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 14n,
    ownerLeaseGeneration: 18n,
    storeVersion: 'store-v2'
  };
  runtime.rememberSpotRoute(successorRoute);
  // A larger owner generation can replace the current owner only for the
  // same object incarnation. The older object route remains stale even when
  // its owner generation is numerically larger.
  runtime.rememberSpotRoute({
    ...firstRoute,
    authorityOwnerGeneration: 12n,
    storeVersion: 'store-stale-owner'
  });
  runtime.forgetSpotRoute(spot, 13n, 'store-v1');
  assert.equal(runtime.sendToSpot('source', successorRoute, payload), SubmitResult.Ok);
  const successorHeader = decodeStatefulHeader(sent.at(-1)!.parts[0]!);
  assert.equal(successorHeader.kind, 'spotSend');
  if (successorHeader.kind === 'spotSend') {
    assert.equal(successorHeader.target.targetNodeRid, 'node-c');
    assert.deepEqual(successorHeader.target, successorRoute);
  }
  runtime.close();
});

test('a new Instance incarnation outranks a reset authority owner generation', () => {
  const raw = {
    topology: { peer: () => undefined },
    setServiceIngress() {},
    sendService: () => true
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-b', 3n);
  const oldDirect = {
    spot: { spotId: 'tenant:incarnation', generation: 1n },
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    authorityOwnerGeneration: 1n,
    ownerLeaseGeneration: 1n,
    storeVersion: 'store-v1'
  };
  const newDirect = {
    ...oldDirect,
    spot: { spotId: oldDirect.spot.spotId, generation: 2n },
    targetNodeRid: 'node-b',
    storeVersion: 'store-v2'
  };
  runtime.rememberSpotRoute(oldDirect);
  runtime.rememberSpotRoute(newDirect);

  const oldIntent: ServiceInstanceRouteFence = {
    targetNodeRid: 'node-b',
    targetNodeGeneration: 3n,
    targetSpotId: oldDirect.spot.spotId,
    objectGeneration: 1n,
    ownerId: 'node-a',
    authorityOwnerGeneration: 1n,
    leaseGeneration: 1n,
    storeVersion: 'store-v1'
  };
  const newIntent: ServiceInstanceRouteFence = {
    ...oldIntent,
    objectGeneration: 2n,
    ownerId: 'node-b',
    storeVersion: 'store-v2'
  };
  runtime.registerInstanceIntent('TenantWorker', oldIntent);
  runtime.registerInstanceIntent('TenantWorker', newIntent);

  // Cleanup from the closed generation must not remove its successor.
  runtime.forgetSpotRoute(oldDirect.spot, oldDirect.authorityOwnerGeneration, oldDirect.storeVersion);
  runtime.forgetInstanceIntent(
    oldIntent.targetSpotId,
    oldIntent.objectGeneration,
    oldIntent.authorityOwnerGeneration,
    oldIntent.storeVersion
  );

  const directRoutes = (runtime as unknown as {
    readonly directSpotRoutes: Map<string, typeof newDirect>;
  }).directSpotRoutes;
  const intents = (runtime as unknown as {
    readonly instanceIntents: Map<string, { readonly route: ServiceInstanceRouteFence }>;
  }).instanceIntents;
  assert.deepEqual(directRoutes.get(newDirect.spot.spotId), newDirect);
  assert.deepEqual(intents.get(newIntent.targetSpotId)?.route, newIntent);

  assert.throws(() => runtime.rememberSpotRoute({
    ...newDirect,
    targetNodeRid: 'node-c',
    storeVersion: 'store-invalid'
  }), ServiceStaleGenerationError);
  assert.throws(() => runtime.registerInstanceIntent('TenantWorker', {
    ...newIntent,
    ownerId: 'node-c',
    storeVersion: 'store-invalid'
  }), ServiceStaleGenerationError);
  runtime.close();
});

test('direct Spot ingress accepts a prior incarnation route for the current Ready object', () => {
  let ingress:
    ((record: import('../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime')
      .RawServiceIngressRecord) => unknown) | undefined;
  const admitted: unknown[] = [];
  const raw = {
    mailbox: {
      tryEnqueue(record: unknown) {
        admitted.push(record);
        return true;
      }
    },
    topology: {
      peer: () => undefined
    },
    setServiceIngress(handler: typeof ingress) {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-a', 3n);
  runtime.restoreSpotAuthority('room', 'user_spot', 'Room', 9n, 13n);
  const current = {
    spot: { spotId: 'room', generation: 9n },
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    authorityOwnerGeneration: 13n,
    ownerLeaseGeneration: 17n,
    storeVersion: 'store-v9'
  };
  runtime.rememberSpotRoute(current);
  const previous = {
    ...current,
    spot: { spotId: 'room', generation: 6n },
    storeVersion: 'store-v6'
  };
  const payload = encodeApplicationPayload({
    packetName: 'CurrentObject',
    contentType: 'application/octet-stream',
    payload: Buffer.from('payload')
  });
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.spotSend,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [encodeSpotHeader('spotSend', 'source-spot', previous), payload]
  }), 'application');
  assert.equal(admitted.length, 1);
  runtime.close();
});

test('direct Spot application admission accepts a StoreVersion-only authority advance', () => {
  for (const operationKind of ['send', 'request'] as const) {
    let ingress:
      ((record: import('../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime')
        .RawServiceIngressRecord) => unknown) | undefined;
    const admitted: unknown[] = [];
    const replies: Buffer[][] = [];
    const raw = {
      mailbox: {
        tryEnqueue(record: unknown) {
          admitted.push(record);
          return true;
        }
      },
      topology: {
        peer: () => undefined
      },
      setServiceIngress(handler: typeof ingress) {
        ingress = handler;
      },
      replyService(_record: unknown, parts: readonly Buffer[]) {
        replies.push([...parts]);
      }
    } as unknown as RawServiceMeshRuntime;
    const runtime = new ServiceStatefulRuntime(raw, 'node-a', 3n);
    runtime.restoreSpotAuthority('room', 'user_spot', 'Room', 9n, 13n);
    const current = {
      spot: { spotId: 'room', generation: 9n },
      targetNodeRid: 'node-a',
      targetNodeGeneration: 3n,
      authorityOwnerGeneration: 13n,
      ownerLeaseGeneration: 17n,
      storeVersion: 'store-v9'
    };
    runtime.rememberSpotRoute(current);
    const cached = { ...current, storeVersion: 'store-v8' };
    const payload = encodeApplicationPayload({
      packetName: 'CurrentObject',
      contentType: 'application/octet-stream',
      payload: Buffer.from('payload')
    });
    const request = operationKind === 'request';
    assert.equal(ingress?.({
      command: request
        ? M6bServiceWireCommand.spotRequest
        : M6bServiceWireCommand.spotSend,
      flags: 0,
      sourceRoutingId: 'source',
      ...(request
        ? {
            sourceRoute: Buffer.from('reply-route'),
            requestSequence: 1n
          }
        : {}),
      parts: [
        encodeSpotHeader(
          request ? 'spotRequest' : 'spotSend',
          'source-spot',
          cached,
          request ? 7n : undefined
        ),
        payload
      ]
    }), 'application');
    assert.equal(admitted.length, 1);
    assert.equal(replies.length, 0);
    runtime.close();
  }
});

test('direct Spot request maps an owner fence mismatch to Unavailable', () => {
  let ingress:
    ((record: import('../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime')
      .RawServiceIngressRecord) => unknown) | undefined;
  const replies: Buffer[][] = [];
  const raw = {
    mailbox: {
      tryEnqueue: () => true
    },
    topology: {
      peer: () => undefined
    },
    setServiceIngress(handler: typeof ingress) {
      ingress = handler;
    },
    replyService(_record: unknown, parts: readonly Buffer[]) {
      replies.push([...parts]);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-a', 3n);
  runtime.restoreSpotAuthority('room', 'user_spot', 'Room', 9n, 13n);
  const route = {
    spot: { spotId: 'room', generation: 9n },
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    authorityOwnerGeneration: 13n,
    ownerLeaseGeneration: 17n,
    storeVersion: 'store-v9'
  };
  runtime.rememberSpotRoute(route);
  const payload = encodeApplicationPayload({
    packetName: 'Ping',
    contentType: 'application/octet-stream',
    payload: Buffer.from('payload')
  });
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.spotRequest,
    flags: 0,
    sourceRoutingId: 'caller',
    sourceRoute: Buffer.from('reply-route'),
    requestSequence: 1n,
    parts: [
      encodeSpotHeader('spotRequest', 'source-spot', {
        ...route,
        ownerLeaseGeneration: route.ownerLeaseGeneration + 1n
      }, 7n),
      payload
    ]
  }), 'infrastructure');
  assert.equal(replies.length, 1);
  assert.deepEqual(decodeStatefulReply(replies[0]![0]!, 7n, 'spotRequest'), {
    correlation: 7n,
    terminalResult: RequestResult.Conflict,
    failureCode: 34
  });
  runtime.close();
});

test('Ready Instance application admission uses the current same-owner incarnation', () => {
  const current: ServiceInstanceRouteFence = {
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    targetSpotId: 'tenant-logical-target',
    objectGeneration: 9n,
    ownerId: 'owner-a',
    authorityOwnerGeneration: 13n,
    leaseGeneration: 17n,
    storeVersion: 'store-v9'
  };
  const harness = readyInstanceIngressHarness(current);
  const previous = {
    ...current,
    objectGeneration: 6n,
    authorityOwnerGeneration: 11n,
    storeVersion: 'store-v6'
  };

  assert.equal(harness.ingress(harness.request(previous, 'send')), 'application');
  assert.equal(harness.queued.length, 1);
  assert.deepEqual(
    (harness.queued[0] as { readonly stateful: { readonly targetSpot: { readonly generation: bigint } } }).stateful.targetSpot,
    { spotId: current.targetSpotId, generation: current.objectGeneration }
  );
  harness.runtime.close();
});

test('Ready Instance admission repairs a local projection published after authority commit', () => {
  const current: ServiceInstanceRouteFence = {
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    targetSpotId: 'tenant-ready-publication-race',
    objectGeneration: 9n,
    ownerId: 'owner-a',
    authorityOwnerGeneration: 13n,
    leaseGeneration: 17n,
    storeVersion: 'store-v9'
  };
  const harness = readyInstanceIngressHarness(current, false);

  assert.equal(harness.ingress(harness.request(current, 'request')), 'application');
  assert.equal(harness.queued.length, 1);
  assert.equal(harness.replies.length, 0);
  harness.runtime.close();
});

test('Ready Instance application admission preserves generation and owner error categories', () => {
  const current: ServiceInstanceRouteFence = {
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    targetSpotId: 'tenant-error-category',
    objectGeneration: 9n,
    ownerId: 'owner-a',
    authorityOwnerGeneration: 13n,
    leaseGeneration: 17n,
    storeVersion: 'store-v9'
  };

  const generationHarness = readyInstanceIngressHarness(current);
  const generationReply = {
    ...current,
    objectGeneration: 10n
  };
  assert.equal(generationHarness.ingress(generationHarness.request(generationReply, 'request')), 'infrastructure');
  assert.equal(generationHarness.replies.length, 1);
  const staleReply = decodeStatefulReply(
    generationHarness.replies[0]![0]!,
    2n,
    'instanceSpotRequest'
  );
  assert.deepEqual(staleReply, {
    correlation: 2n,
    terminalResult: RequestResult.Conflict,
    failureCode: 33
  });
  generationHarness.runtime.close();

  const ownerHarness = readyInstanceIngressHarness(current);
  const moved = {
    ...current,
    objectGeneration: 8n,
    ownerId: 'owner-b'
  };
  assert.equal(ownerHarness.ingress(ownerHarness.request(moved, 'request')), 'infrastructure');
  const movedReply = decodeStatefulReply(
    ownerHarness.replies[0]![0]!,
    2n,
    'instanceSpotRequest'
  );
  assert.deepEqual(movedReply, {
    correlation: 2n,
    terminalResult: RequestResult.Conflict,
    failureCode: 34
  });
  ownerHarness.runtime.close();

  const missingHarness = readyInstanceIngressHarness();
  assert.equal(
    missingHarness.ingress(missingHarness.request({ ...current, objectGeneration: 1n }, 'request')),
    'infrastructure'
  );
  const missingReply = decodeStatefulReply(
    missingHarness.replies[0]![0]!,
    2n,
    'instanceSpotRequest'
  );
  assert.deepEqual(missingReply, {
    correlation: 2n,
    terminalResult: RequestResult.NotFound,
    failureCode: 14
  });
  missingHarness.runtime.close();

  const storeHarness = readyInstanceIngressHarness(current);
  const storeMismatch = { ...current, storeVersion: 'store-v8' };
  assert.equal(
    storeHarness.ingress(storeHarness.request(storeMismatch, 'request')),
    'application'
  );
  assert.equal(storeHarness.queued.length, 1);
  assert.equal(storeHarness.replies.length, 0);
  storeHarness.runtime.close();
});

test('remote Entry Spot actor join derives the well-known node route fence', async () => {
  const requests: Array<{ readonly target: string; readonly header: ReturnType<typeof decodeStatefulHeader> }> = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'entry-node'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    mailbox: {
      tryEnqueue: () => true
    },
    setServiceIngress: () => {},
    async requestService(target: string, parts: readonly Buffer[]) {
      const header = decodeStatefulHeader(parts[0]!);
      requests.push({ target, header });
      assert.equal(header.kind, 'actorJoin');
      if (header.kind !== 'actorJoin') throw new Error('expected actor join header');
      return [encodeStatefulReply(header.correlation, RequestResult.Ok, 0, {
        kind: 'actorJoin',
        joinResult: 0,
        spot: { spotId: 'entry-node', generation: 7n },
        membershipEpoch: 2n
      })];
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'actor-node', 3n);
  const actor = runtime.createActor('remote-entry-actor').ref;

  const result = await runtime.joinActorEntrySpot(
    actor,
    'entry-node',
    undefined,
    1_000
  ).promise;

  assert.equal(result.terminalResult, RequestResult.Ok);
  assert.equal(requests.length, 1);
  assert.equal(requests[0]!.target, 'entry-node');
  assert.equal(requests[0]!.header.kind, 'actorJoin');
  if (requests[0]!.header.kind === 'actorJoin') {
    assert.equal(requests[0]!.header.entry, true);
    assert.deepEqual(requests[0]!.header.target, {
      spot: { spotId: 'entry-node', generation: 7n },
      targetNodeRid: 'entry-node',
      targetNodeGeneration: 7n,
      authorityOwnerGeneration: 7n
    });
  }
  runtime.close();
});

test('Spot Message Follow holds ingress, relays with the committed fence, and restores on abort', async () => {
  let ingress:
    ((record: import('../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime')
      .RawServiceIngressRecord) => unknown) | undefined;
  const relayed: Array<{ readonly target: string; readonly parts: readonly Buffer[] }> = [];
  const restored: unknown[] = [];
  const replies: Buffer[][] = [];
  const raw = {
    mailbox: {
      tryEnqueue(record: unknown) {
        restored.push(record);
        return true;
      }
    },
    setServiceIngress(handler: typeof ingress) {
      ingress = handler;
    },
    sendService(target: string, parts: readonly Buffer[]) {
      relayed.push({ target, parts });
      return true;
    },
    async requestService(_target: string, _parts: readonly Buffer[]) {
      return [
        encodeStatefulReply(91n, RequestResult.Ok, 0)
      ];
    },
    replyService(_record: unknown, parts: readonly Buffer[]) {
      replies.push([...parts]);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-a', 3n);
  runtime.restoreSpotAuthority('room', 'user_spot', 'Room', 5n, 7n);
  const source = {
    spot: { spotId: 'room', generation: 5n },
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    authorityOwnerGeneration: 7n,
    ownerLeaseGeneration: 3n,
    storeVersion: 'source-v1'
  };
  runtime.rememberSpotRoute(source);
  const payload = encodeApplicationPayload({
    packetName: 'Move',
    contentType: 'application/octet-stream',
    payload: Buffer.from('payload')
  });
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.spotSend,
    flags: 0,
    sourceRoutingId: 'caller',
    parts: [encodeSpotHeader('spotSend', 'source-spot', {
      ...source,
      ownerLeaseGeneration: source.ownerLeaseGeneration + 1n
    }), payload]
  }), 'protocolError');
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.spotSend,
    flags: 0,
    sourceRoutingId: 'caller',
    parts: [encodeSpotHeader('spotSend', 'source-spot', {
      ...source,
      storeVersion: 'stale-store-version'
    }), payload]
  }), 'application');
  const seal = runtime.sealSpotMessageFollowIngress(source);
  assert.ok(seal);
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.spotSend,
    flags: 0,
    sourceRoutingId: 'caller',
    parts: [encodeSpotHeader('spotSend', 'source-spot', source), payload]
  }), 'application');
  assert.equal(relayed.length, 0);

  const target = {
    spot: source.spot,
    targetNodeRid: 'node-b',
    targetNodeGeneration: 9n,
    authorityOwnerGeneration: 8n,
    ownerLeaseGeneration: 4n,
    storeVersion: 'target-v1'
  };
  assert.equal(
    await runtime.commitSpotMessageFollowIngress(seal, target, 30_000),
    true
  );
  assert.equal(relayed.length, 2);
  const relayedHeader = decodeStatefulHeader(
    relayed.find(record => record.target === 'node-b')!.parts[0]!
  );
  assert.equal(relayedHeader.kind, 'spotSend');
  if (relayedHeader.kind === 'spotSend') {
    assert.deepEqual(relayedHeader.target, target);
    assert.equal(relayedHeader.sourceSpotId, 'source-spot');
  }
  const sendFollow = decodeStatefulHeader(
    relayed.find(record => record.target === 'caller')!.parts[0]!
  );
  assert.equal(sendFollow.kind, 'messageFollow');
  if (sendFollow.kind === 'messageFollow') {
    assert.deepEqual(sendFollow.source, {
      kind: 'spot',
      spot: source.spot,
      targetNodeRid: source.targetNodeRid,
      targetNodeGeneration: source.targetNodeGeneration,
      authorityOwnerGeneration: source.authorityOwnerGeneration,
      ownerLeaseGeneration: source.ownerLeaseGeneration
    });
    assert.deepEqual(sendFollow.target, {
      kind: 'spot',
      spot: target.spot,
      targetNodeRid: target.targetNodeRid,
      targetNodeGeneration: target.targetNodeGeneration,
      authorityOwnerGeneration: target.authorityOwnerGeneration,
      ownerLeaseGeneration: target.ownerLeaseGeneration
    });
    assert.equal(sendFollow.queuedMessages, 1);
    assert.equal(sendFollow.originalReplyRouteId, 0n);
  }

  assert.equal(ingress?.({
    command: M6bServiceWireCommand.spotRequest,
    flags: 0,
    sourceRoutingId: 'caller',
    sourceRoute: Buffer.from('reply-route'),
    requestSequence: 17n,
    parts: [encodeSpotHeader('spotRequest', 'source-spot', source, 91n), payload]
  }), 'application');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(replies.length, 1);
  assert.deepEqual(decodeStatefulReply(replies[0]![0]!, 91n, 'spotRequest'), {
    correlation: 91n,
    terminalResult: RequestResult.Ok,
    failureCode: 0
  });
  const requestFollow = decodeStatefulHeader(
    relayed.filter(record => record.target === 'caller').at(-1)!.parts[0]!
  );
  assert.equal(requestFollow.kind, 'messageFollow');
  if (requestFollow.kind === 'messageFollow') {
    assert.equal(requestFollow.originalOperation.low, 17n);
    assert.equal(requestFollow.originalReplyRouteId, 17n);
  }

  runtime.restoreSpotAuthority('abort-room', 'user_spot', 'Room', 6n, 11n);
  const abortSource = {
    spot: { spotId: 'abort-room', generation: 6n },
    targetNodeRid: 'node-a',
    targetNodeGeneration: 3n,
    authorityOwnerGeneration: 11n,
    ownerLeaseGeneration: 3n,
    storeVersion: 'abort-v1'
  };
  runtime.rememberSpotRoute(abortSource);
  const abortSeal = runtime.sealSpotMessageFollowIngress(abortSource);
  assert.ok(abortSeal);
  assert.equal(ingress?.({
    command: M6bServiceWireCommand.spotSend,
    flags: 0,
    sourceRoutingId: 'caller',
    parts: [encodeSpotHeader('spotSend', 'source-spot', abortSource), payload]
  }), 'application');
  assert.equal(runtime.abortSpotMessageFollowIngress(abortSeal), true);
  assert.equal(restored.length, 2);
  runtime.close();
});

test('Instance activation encoding distinguishes absent metadata from explicit empty metadata', () => {
  const sent: Array<{ readonly target: string; readonly parts: readonly Buffer[] }> = [];
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    setServiceIngress: () => {},
    isPeerRouteReady: () => true,
    sendService: (target: string, parts: readonly Buffer[]) => {
      sent.push({ target, parts });
      return false;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source', 3n);
  const target = {
    targetNodeRid: 'target',
    targetNodeGeneration: 7n,
    targetSpotId: 'room',
    stableType: 'Room',
    descriptorVersion: '1'
  };
  const payload = {
    packetName: 'Open',
    contentType: 'application/octet-stream',
    payload: Buffer.from('open')
  };
  assert.equal(runtime.sendToMissingInstanceSpot(
    target,
    payload,
    BigInt(Date.now() + 1_000)
  ), SubmitResult.Backpressured);
  assert.equal(runtime.sendToMissingInstanceSpot(
    target,
    payload,
    BigInt(Date.now() + 1_000),
    undefined,
    encodeServiceMetadataFrame(new Map())
  ), SubmitResult.Backpressured);
  assert.equal(sent[0]?.parts.length, 2);
  assert.equal(sent[1]?.parts.length, 3);
  assert.deepEqual(
    validateServiceMetadataFrame(sent[1]!.parts[1]!),
    Buffer.from([1, 0])
  );
  runtime.close();
});

test('Missing Instance bounded admission reuses one immutable operation envelope', () => {
  const sent: Array<readonly Buffer[]> = [];
  let attempts = 0;
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    setServiceIngress: () => {},
    isPeerRouteReady: () => true,
    sendService: (_target: string, parts: readonly Buffer[]) => {
      sent.push(parts);
      attempts += 1;
      return attempts > 1;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source', 3n);
  const target = {
    targetNodeRid: 'target',
    targetNodeGeneration: 7n,
    targetSpotId: 'room',
    stableType: 'Room',
    descriptorVersion: '1'
  };
  const submit = runtime.prepareMissingInstanceSpotSend(
    target,
    {
      packetName: 'Open',
      contentType: 'application/octet-stream',
      payload: Buffer.from('open')
    },
    BigInt(Date.now() + 1_000)
  );

  assert.equal(submit(), SubmitResult.Backpressured);
  assert.equal(submit(), SubmitResult.Ok);
  assert.equal(sent.length, 2);
  assert.strictEqual(sent[0], sent[1]);
  const first = decodeStatefulHeader(sent[0]![0]!);
  const second = decodeStatefulHeader(sent[1]![0]!);
  assert.equal(first.kind, 'instanceSpot');
  assert.equal(second.kind, 'instanceSpot');
  if (first.kind === 'instanceSpot' && second.kind === 'instanceSpot') {
    assert.deepEqual(first.operation, second.operation);
  }
  runtime.close();
});

test('retained peer routes may submit while endpoint convergence reports not ready', () => {
  const sent: Array<{ readonly target: string; readonly parts: readonly Buffer[] }> = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'retained-peer'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    setServiceIngress: () => {},
    isPeerRouteReady: () => false,
    sendService: (target: string, parts: readonly Buffer[]) => {
      sent.push({ target, parts });
      return true;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'source', 3n);
  const actor = {
    nodeRid: 'retained-peer',
    actorId: 'actor-retained-route',
    generation: 5n
  };
  runtime.rememberActorRoute({
    actor,
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 11n
  });

  const result = runtime.sendToActor(actor, 7n, 5n, {
    packetName: 'RetainedRoute',
    contentType: 'application/octet-stream',
    payload: Buffer.from('payload')
  });

  assert.equal(result, SubmitResult.Ok);
  assert.equal(sent.length, 1);
  assert.equal(sent[0]!.target, 'retained-peer');
  runtime.close();
});

test('global Spot and Actor identities fence stale generations and retain stable type', () => {
  const registry = new ServiceStatefulRegistry('node-a', 4n);
  const spotV1 = registry.createSpot('spot-a', 'user', 'Room');
  const actorV1 = registry.createActor('actor-a', 'Player', spotV1.ref);
  assert.equal(registry.closeSpot(spotV1.ref), false);
  registry.destroyActor(actorV1.ref);
  assert.equal(registry.closeSpot(spotV1.ref), true);

  const spotV2 = registry.createSpot('spot-a', 'user', 'Room');
  const actorV2 = registry.createActor('actor-a', 'Player', spotV2.ref);
  assert.equal(spotV2.ref.generation, 2n);
  assert.equal(actorV2.ref.generation, 2n);
  assert.throws(() => registry.requireSpot(spotV1.ref), ServiceStaleGenerationError);
  assert.throws(() => registry.requireActor(actorV1.ref), ServiceStaleGenerationError);
  registry.destroyActor(actorV2.ref);
  assert.throws(() => registry.createActor('actor-a', 'Enemy', spotV2.ref), TypeError);
});

test('remote create reservations are idempotent per attempt and fence stale attempts', () => {
  const target = new ServiceStatefulRegistry('node-target', 2n);
  const first = target.reserve('instanceSpot', 'tenant-42', 'TenantWorker', 10n);
  assert.equal(first.kind, 'reserved');
  if (first.kind !== 'reserved') return;
  const duplicate = target.reserve('instanceSpot', 'tenant-42', 'TenantWorker', 10n);
  assert.deepEqual(duplicate, first);

  const newer = target.reserve('instanceSpot', 'tenant-42', 'TenantWorker', 11n);
  assert.equal(newer.kind, 'reserved');
  if (newer.kind !== 'reserved') return;
  assert.equal(target.reserve('instanceSpot', 'tenant-42', 'TenantWorker', 10n).kind, 'attemptStale');
  assert.throws(() => target.commitReservation(first.reservation), ServiceStaleGenerationError);
  const committed = target.commitReservation(newer.reservation);
  assert.equal('kind' in committed ? committed.kind : undefined, 'instance');
  assert.equal(target.reserve('instanceSpot', 'tenant-42', 'OtherWorker', 12n).kind, 'typeMismatch');
});

test('target-owned Instance activation reserves before factory, commits before one queue admission', () => {
  const queued: unknown[] = [];
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  const events: string[] = [];
  let committedRoute: ReturnType<ServiceInstanceActivationAuthority['read']> = { kind: 'missing' };
  const authority: ServiceInstanceActivationAuthority = {
    read: () => {
      events.push('read');
      return committedRoute;
    },
    reserve: () => {
      events.push('reserve');
      assert.equal(runtime.registry.spot('tenant-42'), undefined);
      return {
        kind: 'reserved',
        reservation: {
          attempt: 11n,
          authorityOwnerGeneration: 17n,
          token: 'reservation-11'
        }
      };
    },
    commit: (_target, _reservation, spot) => {
      events.push('commit');
      assert.equal(runtime.registry.spot('tenant-42'), spot);
      assert.equal(spot.ref.generation, 11n);
      assert.equal(spot.authorityOwnerGeneration, 17n);
      const committed = {
        kind: 'committed',
        route: {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-42',
          objectGeneration: spot.ref.generation,
          ownerId: 'target',
          authorityOwnerGeneration: spot.authorityOwnerGeneration,
          leaseGeneration: 1n,
          storeVersion: '12'
        }
      } as const;
      committedRoute = { kind: 'ready', route: committed.route };
      return committed;
    },
    abort: () => {
      assert.fail('successful activation must not abort');
    }
  };
  runtime.registerInstanceActivationAuthority(authority);

  const target = {
    targetNodeRid: 'target',
    targetNodeGeneration: 3n,
    targetSpotId: 'tenant-42',
    stableType: 'TenantWorker',
    descriptorVersion: 'descriptor-5'
  };
  const header = encodeInstanceSpotActivationHeader(
    target,
    7n,
    'source',
    undefined,
    'send',
    { high: 7n, low: 31n },
    BigInt(Date.now() + 10_000)
  );
  const parts = [
    header,
    encodeApplicationPayload({
      packetName: 'FirstMessage',
      contentType: 'application/octet-stream',
      payload: Buffer.from('first')
    })
  ];
  const record = {
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts
  };
  assert.equal(ingress(record), 'application');
  assert.deepEqual(events, ['read', 'reserve', 'commit']);
  assert.equal(runtime.registry.spot('tenant-42')?.stableType, 'TenantWorker');
  assert.equal(queued.length, 1);

  assert.equal(ingress(record), 'application');
  assert.deepEqual(events, ['read', 'reserve', 'commit', 'read']);
  assert.equal(queued.length, 1);
  runtime.close();
});

test('durable missing Instance authority replaces an unmaterialized local projection', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const queued: unknown[] = [];
  const events: string[] = [];
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  runtime.restoreSpotAuthority('tenant-orphan', 'instance_spot', 'TenantWorker', 1n, 1n);
  runtime.registerInstanceApplicationLifecycle({
    isMaterialized: target => {
      events.push(`check:${target.targetSpotId}`);
      return false;
    },
    materialize: (target, generation) => {
      events.push(`materialize:${target.targetSpotId}:${String(generation)}`);
      return Promise.resolve();
    },
    discard: async () => undefined,
    beginTerminal: () => undefined,
    completeTerminal: async () => false
  });
  const authority: ServiceAsyncInstanceActivationAuthority = {
    read: async () => ({ kind: 'missing' }),
    reserve: async activation => {
      events.push('reserve');
      assert.equal(runtime.registry.spot(activation.target.targetSpotId)?.ref.generation, 1n);
      return {
        kind: 'reserved',
        reservation: {
          attempt: 2n,
          authorityOwnerGeneration: 2n,
          token: 'reservation-orphan'
        }
      };
    },
    resume: async () => assert.fail('Live activation must not resume a startup reservation'),
    commit: async (_target, _reservation, spot) => {
      events.push('commit');
      assert.equal(spot.ref.generation, 2n);
      return {
        kind: 'committed',
        route: {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-orphan',
          objectGeneration: 2n,
          ownerId: 'target',
          authorityOwnerGeneration: 2n,
          leaseGeneration: 1n,
          storeVersion: 'orphan-v2'
        }
      };
    },
    complete: async () => assert.fail('The admitted message has not completed yet'),
    abort: async () => assert.fail('Successful activation must not abort')
  };
  runtime.registerAsyncInstanceActivationAuthority(authority);

  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotActivationHeader(
        {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-orphan',
          stableType: 'TenantWorker',
          descriptorVersion: 'descriptor-orphan'
        },
        7n,
        'source',
        undefined,
        'send',
        { high: 7n, low: 45n },
        BigInt(Date.now() + 10_000)
      ),
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('first')
      })
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.deepEqual(events, [
    'check:tenant-orphan',
    'reserve',
    'materialize:tenant-orphan:2',
    'commit'
  ]);
  assert.equal(runtime.registry.spot('tenant-orphan')?.ref.generation, 2n);
  assert.equal(queued.length, 1);
  runtime.close();
});

test('Instance activation joins a Creating authority when local materialization is still pending', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const queued: unknown[] = [];
  const events: string[] = [];
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  runtime.activateInstanceSpot('tenant-overlap', 'TenantWorker', 4n, 9n);
  runtime.registerInstanceApplicationLifecycle({
    isMaterialized: () => true,
    isMaterializing: () => true,
    materialize: (target, generation) => {
      events.push(`materialize:${target.targetSpotId}:${String(generation)}`);
      return Promise.resolve();
    },
    discard: async () => undefined,
    beginTerminal: () => undefined,
    completeTerminal: async () => false
  });
  runtime.registerAsyncInstanceActivationAuthority({
    read: async () => {
      events.push('read');
      return { kind: 'missing' };
    },
    reserve: async () => {
      events.push('reserve');
      return {
        kind: 'reserved',
        reservation: {
          attempt: 4n,
          authorityOwnerGeneration: 9n,
          token: 'reservation-overlap'
        }
      };
    },
    resume: async () => assert.fail('Live activation must not resume a startup reservation'),
    commit: async (_target, _reservation, spot) => {
      events.push('commit');
      return {
        kind: 'committed',
        route: {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-overlap',
          objectGeneration: spot.ref.generation,
          ownerId: 'target',
          authorityOwnerGeneration: spot.authorityOwnerGeneration,
          leaseGeneration: 1n,
          storeVersion: 'overlap-v1'
        }
      };
    },
    complete: async () => assert.fail('The admitted message has not completed yet'),
    abort: async () => assert.fail('Successful activation must not abort')
  });

  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotActivationHeader(
        {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-overlap',
          stableType: 'TenantWorker',
          descriptorVersion: 'descriptor-overlap'
        },
        7n,
        'source',
        undefined,
        'send',
        { high: 7n, low: 46n },
        BigInt(Date.now() + 10_000)
      ),
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('first')
      })
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.deepEqual(events, [
    'read',
    'reserve',
    'materialize:tenant-overlap:4',
    'commit'
  ]);
  assert.equal(queued.length, 1);
  runtime.close();
});

test('Instance activation joins a Creating authority after local materialization completes', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const queued: unknown[] = [];
  const events: string[] = [];
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  runtime.activateInstanceSpot('tenant-creating', 'TenantWorker', 4n, 9n);
  runtime.registerInstanceApplicationLifecycle({
    isMaterialized: () => true,
    isMaterializing: () => false,
    materialize: () => {
      events.push('materialize');
      return Promise.resolve();
    },
    discard: async () => undefined,
    beginTerminal: () => undefined,
    completeTerminal: async () => false
  });
  runtime.registerAsyncInstanceActivationAuthority({
    read: async () => ({
      kind: 'creating',
      objectGeneration: 4n,
      authorityOwnerGeneration: 9n
    }),
    reserve: async () => {
      events.push('reserve');
      return {
        kind: 'reserved',
        reservation: {
          attempt: 4n,
          authorityOwnerGeneration: 9n,
          token: 'reservation-creating'
        }
      };
    },
    resume: async () => assert.fail('Live activation must not resume a startup reservation'),
    commit: async (_target, _reservation, spot) => {
      events.push('commit');
      return {
        kind: 'committed',
        route: {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-creating',
          objectGeneration: spot.ref.generation,
          ownerId: 'target',
          authorityOwnerGeneration: spot.authorityOwnerGeneration,
          leaseGeneration: 1n,
          storeVersion: 'creating-v1'
        }
      };
    },
    complete: async () => assert.fail('The admitted message has not completed yet'),
    abort: async () => assert.fail('Successful activation must not abort')
  });

  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotActivationHeader(
        {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-creating',
          stableType: 'TenantWorker',
          descriptorVersion: 'descriptor-creating'
        },
        7n,
        'source',
        undefined,
        'send',
        { high: 7n, low: 47n },
        BigInt(Date.now() + 10_000)
      ),
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('first')
      })
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.deepEqual(events, ['reserve', 'materialize', 'commit']);
  assert.equal(queued.length, 1);
  runtime.close();
});

test('Ready Instance route waits for a closing materialized application before admission', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const queued: unknown[] = [];
  const events: string[] = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  const route: ServiceInstanceRouteFence = {
    targetNodeRid: 'target',
    targetNodeGeneration: 3n,
    targetSpotId: 'tenant-rematerialize',
    objectGeneration: 4n,
    ownerId: 'target',
    authorityOwnerGeneration: 12n,
    leaseGeneration: 2n,
    storeVersion: 'route-4'
  };
  runtime.registerInstanceIntent('TenantWorker', route);
  runtime.registerInstanceApplicationLifecycle({
    isClosing: target => {
      events.push(`closing:${target.targetSpotId}`);
      return true;
    },
    isMaterialized: target => {
      events.push(`check:${target.targetSpotId}`);
      return true;
    },
    materialize: (target, generation) => {
      events.push(`materialize:${target.stableType}:${String(generation)}`);
      return Promise.resolve();
    },
    discard: async () => undefined,
    beginTerminal: () => undefined,
    completeTerminal: async () => false
  });

  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotHeader(
        route,
        7n,
        'source',
        undefined,
        'send',
        { high: 0n, low: 0n }
      ),
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('first')
      })
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.deepEqual(events, [
    'closing:tenant-rematerialize',
    'materialize:TenantWorker:4'
  ]);
  assert.equal(queued.length, 1);
  assert.equal(runtime.registry.spot('tenant-rematerialize')?.stableType, 'TenantWorker');
  runtime.close();
});

test('Instance activation CAS loser does not invoke the local factory', () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    mailbox: { tryEnqueue: () => true },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  const winnerRoute = {
    targetNodeRid: 'other-target',
    targetNodeGeneration: 8n,
    targetSpotId: 'tenant-42',
    objectGeneration: 2n,
    ownerId: 'other-target',
    authorityOwnerGeneration: 9n,
    leaseGeneration: 4n,
    storeVersion: '19'
  };
  runtime.registerInstanceActivationAuthority({
    read: () => ({ kind: 'missing' }),
    reserve: () => ({ kind: 'ready', route: winnerRoute }),
    commit: () => assert.fail('CAS loser must not commit'),
    abort: () => assert.fail('CAS loser must not abort another owner')
  });
  const header = encodeInstanceSpotActivationHeader(
    {
      targetNodeRid: 'target',
      targetNodeGeneration: 3n,
      targetSpotId: 'tenant-42',
      stableType: 'TenantWorker',
      descriptorVersion: 'descriptor-5'
    },
    7n,
    'source',
    undefined,
    'send',
    { high: 7n, low: 32n },
    BigInt(Date.now() + 10_000)
  );
  assert.throws(
    () => ingress({
      command: M6bServiceWireCommand.instanceSpot,
      flags: 0,
      sourceRoutingId: 'source',
      parts: [
        header,
        encodeApplicationPayload({
          packetName: 'FirstMessage',
          contentType: 'application/octet-stream',
          payload: Buffer.from('first')
        })
      ]
    }),
    ServiceInstanceActivationRedirectError
  );
  assert.equal(runtime.registry.spot('tenant-42'), undefined);
  runtime.close();
});

test('Promise authority redirects the retained activation envelope to the Ready winner', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  let redirected:
    | { readonly targetNodeRid: string; readonly parts: readonly Buffer[] }
    | undefined;
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    mailbox: { tryEnqueue: () => assert.fail('CAS loser must not admit locally') },
    sendService: (targetNodeRid: string, parts: readonly Buffer[]) => {
      redirected = { targetNodeRid, parts };
      return true;
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'loser', 3n);
  runtime.registerAsyncInstanceActivationAuthority({
    read: async () => ({
      kind: 'ready',
      route: {
        targetNodeRid: 'winner',
        targetNodeGeneration: 9n,
        targetSpotId: 'tenant-redirect',
        objectGeneration: 4n,
        ownerId: 'winner-owner',
        authorityOwnerGeneration: 6n,
        leaseGeneration: 2n,
        storeVersion: 'ready-v1'
      }
    }),
    reserve: async () => assert.fail('Ready authority must not reserve'),
    resume: async () => assert.fail('Ready authority must not resume'),
    commit: async () => assert.fail('Ready authority must not commit'),
    complete: async () => assert.fail('Redirected activation must not complete locally'),
    abort: async () => assert.fail('Ready authority must not abort')
  });

  const operation = { high: 7n, low: 44n };
  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotActivationHeader(
        {
          targetNodeRid: 'loser',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-redirect',
          stableType: 'TenantWorker',
          descriptorVersion: 'descriptor-loser'
        },
        7n,
        'source',
        'source-spot',
        'send',
        operation,
        BigInt(Date.now() + 10_000)
      ),
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('first')
      })
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.equal(redirected?.targetNodeRid, 'winner');
  if (redirected === undefined) throw new Error('Activation envelope was not redirected.');
  const redirectedHeader = decodeStatefulHeader(redirected.parts[0]!);
  assert.equal(redirectedHeader.kind, 'instanceSpot');
  if (redirectedHeader.kind !== 'instanceSpot') throw new Error('Redirect header is invalid.');
  assert.equal(redirectedHeader.activation, 'missing');
  assert.deepEqual(redirectedHeader.operation, operation);
  assert.equal(redirectedHeader.sourceNodeRid, 'loser');
  assert.equal(redirectedHeader.sourceNodeGeneration, 3n);
  assert.equal(redirectedHeader.sourceSpotId, 'source-spot');
  if (redirectedHeader.activation === 'missing') {
    assert.equal(redirectedHeader.target.targetNodeRid, 'winner');
    assert.equal(redirectedHeader.target.targetNodeGeneration, 9n);
  }
  runtime.close();
});

test('Missing Instance activation joins a new reservation while the prior local generation is closing', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  let redirected:
    | { readonly targetNodeRid: string; readonly parts: readonly Buffer[] }
    | undefined;
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    mailbox: { tryEnqueue: () => assert.fail('Closing local generation must not admit the old projection') },
    sendService: (targetNodeRid: string, parts: readonly Buffer[]) => {
      redirected = { targetNodeRid, parts };
      return true;
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  runtime.restoreSpotAuthority('tenant-close-race', 'instance_spot', 'TenantWorker', 1n, 1n);
  runtime.registerInstanceApplicationLifecycle({
    isClosing: () => true,
    isMaterialized: () => true,
    materialize: async () => assert.fail('A concurrent reservation must be joined, not materialized here'),
    discard: async () => undefined,
    beginTerminal: () => undefined,
    completeTerminal: async () => false
  });
  const winnerRoute: ServiceInstanceRouteFence = {
    targetNodeRid: 'winner',
    targetNodeGeneration: 9n,
    targetSpotId: 'tenant-close-race',
    objectGeneration: 2n,
    ownerId: 'winner-owner',
    authorityOwnerGeneration: 4n,
    leaseGeneration: 2n,
    storeVersion: 'winner-v2'
  };
  runtime.registerAsyncInstanceActivationAuthority({
    read: async () => ({
      kind: 'creating',
      objectGeneration: 2n,
      authorityOwnerGeneration: 4n
    }),
    reserve: async () => ({ kind: 'ready', route: winnerRoute }),
    resume: async () => assert.fail('Live activation must not resume a startup reservation'),
    commit: async () => assert.fail('The concurrent winner must be used'),
    complete: async () => assert.fail('The redirect must not complete locally'),
    abort: async () => assert.fail('The redirect must not abort another owner')
  });

  const operation = { high: 7n, low: 48n };
  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotActivationHeader(
        {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-close-race',
          stableType: 'TenantWorker',
          descriptorVersion: 'descriptor-close-race'
        },
        7n,
        'source',
        undefined,
        'send',
        operation,
        BigInt(Date.now() + 10_000)
      ),
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('close-race-payload')
      })
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.equal(redirected?.targetNodeRid, 'winner');
  if (redirected === undefined) throw new Error('Closing-generation activation was not redirected.');
  const redirectedHeader = decodeStatefulHeader(redirected.parts[0]!);
  assert.equal(redirectedHeader.kind, 'instanceSpot');
  if (redirectedHeader.kind !== 'instanceSpot') throw new Error('Redirect header is invalid.');
  assert.equal(redirectedHeader.activation, 'missing');
  assert.deepEqual(redirectedHeader.operation, operation);
  runtime.close();
});

test('stale local Instance projection redirects to a newer remote Ready authority', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  let redirected:
    | { readonly targetNodeRid: string; readonly parts: readonly Buffer[] }
    | undefined;
  let materializeCalls = 0;
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    mailbox: { tryEnqueue: () => assert.fail('Remote Ready must not admit locally') },
    sendService: (targetNodeRid: string, parts: readonly Buffer[]) => {
      redirected = { targetNodeRid, parts };
      return true;
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  runtime.restoreSpotAuthority('tenant-remote-ready', 'instance_spot', 'TenantWorker', 1n, 1n);
  runtime.registerInstanceApplicationLifecycle({
    isMaterialized: () => false,
    materialize: async () => {
      materializeCalls += 1;
      assert.fail('Remote Ready must not materialize locally');
    },
    discard: async () => undefined,
    beginTerminal: () => undefined,
    completeTerminal: async () => false
  });
  const route: ServiceInstanceRouteFence = {
    targetNodeRid: 'remote',
    targetNodeGeneration: 8n,
    targetSpotId: 'tenant-remote-ready',
    objectGeneration: 2n,
    ownerId: 'remote-owner',
    authorityOwnerGeneration: 4n,
    leaseGeneration: 5n,
    storeVersion: 'remote-ready-v2'
  };
  runtime.registerAsyncInstanceActivationAuthority({
    read: async () => ({ kind: 'ready', route }),
    reserve: async () => assert.fail('Remote Ready must not reserve'),
    resume: async () => assert.fail('Remote Ready must not resume'),
    commit: async () => assert.fail('Remote Ready must not commit'),
    complete: async () => assert.fail('Redirected activation must not complete locally'),
    abort: async () => assert.fail('Remote Ready must not abort')
  });

  const operation = { high: 9n, low: 1n };
  const deadline = BigInt(Date.now() + 10_000);
  const metadata = encodeServiceMetadataFrame(new Map([['trace', 'remote-ready']]));
  const payload = encodeApplicationPayload({
    packetName: 'FirstMessage',
    contentType: 'application/octet-stream',
    payload: Buffer.from('remote-ready-payload')
  });
  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: M6bServiceWireFlag.metadata,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotActivationHeader(
        {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-remote-ready',
          stableType: 'TenantWorker',
          descriptorVersion: 'descriptor-remote-ready'
        },
        7n,
        'source',
        undefined,
        'send',
        operation,
        deadline,
        undefined,
        true
      ),
      metadata,
      payload
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.equal(materializeCalls, 0);
  assert.equal(redirected?.targetNodeRid, 'remote');
  if (redirected === undefined) throw new Error('Remote Ready activation was not redirected.');
  const redirectedHeader = decodeStatefulHeader(redirected.parts[0]!);
  assert.equal(redirectedHeader.kind, 'instanceSpot');
  if (redirectedHeader.kind !== 'instanceSpot') throw new Error('Redirect header is invalid.');
  assert.equal(redirectedHeader.activation, 'missing');
  assert.deepEqual(redirectedHeader.operation, operation);
  assert.equal(redirectedHeader.deadlineUnixMs, deadline);
  assert.deepEqual(redirected.parts[1], metadata);
  assert.deepEqual(redirected.parts[2], payload);
  runtime.close();
});

test('stale local Instance projection reconciles a newer same-node Ready authority once', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const queued: unknown[] = [];
  const sent: string[] = [];
  const events: string[] = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    sendService: (targetNodeRid: string) => {
      sent.push(targetNodeRid);
      return true;
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  runtime.restoreSpotAuthority('tenant-same-node-ready', 'instance_spot', 'TenantWorker', 1n, 1n);
  runtime.registerInstanceApplicationLifecycle({
    isMaterialized: () => false,
    materialize: async (target, generation) => {
      events.push(`materialize:${target.targetSpotId}:${String(generation)}`);
    },
    discard: async () => undefined,
    beginTerminal: () => undefined,
    completeTerminal: async () => false
  });
  const route: ServiceInstanceRouteFence = {
    targetNodeRid: 'target',
    targetNodeGeneration: 3n,
    targetSpotId: 'tenant-same-node-ready',
    objectGeneration: 2n,
    ownerId: 'target-owner',
    authorityOwnerGeneration: 4n,
    leaseGeneration: 5n,
    storeVersion: 'same-node-ready-v2'
  };
  let reads = 0;
  runtime.registerAsyncInstanceActivationAuthority({
    read: async () => {
      reads += 1;
      events.push(`read:${reads}`);
      return { kind: 'ready', route };
    },
    reserve: async () => assert.fail('Ready authority must not reserve'),
    resume: async () => assert.fail('Ready authority must not resume'),
    commit: async () => assert.fail('Ready authority must not commit'),
    complete: async () => assert.fail('The admitted message has not completed yet'),
    abort: async () => assert.fail('Ready authority must not abort')
  });

  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      encodeInstanceSpotActivationHeader(
        {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-same-node-ready',
          stableType: 'TenantWorker',
          descriptorVersion: 'descriptor-same-node-ready'
        },
        7n,
        'source',
        undefined,
        'send',
        { high: 9n, low: 2n },
        BigInt(Date.now() + 10_000)
      ),
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('same-node-ready-payload')
      })
    ]
  }), 'infrastructure');
  await new Promise<void>(resolve => setImmediate(resolve));

  assert.deepEqual(events, [
    'read:1',
    'materialize:tenant-same-node-ready:2',
    'read:2'
  ]);
  assert.equal(reads, 2);
  assert.deepEqual(sent, []);
  assert.equal(queued.length, 1);
  const local = runtime.registry.spot('tenant-same-node-ready');
  assert.ok(local);
  assert.equal(local.ref.generation, route.objectGeneration);
  assert.equal(local.authorityOwnerGeneration, route.authorityOwnerGeneration);
  runtime.close();
});

test('Promise authority resumes the retained activation envelope after Store completion', async () => {
  let ingress!: (record: {
    readonly command: number;
    readonly flags: number;
    readonly sourceRoutingId: string;
    readonly parts: readonly Buffer[];
  }) => string | undefined;
  const queued: unknown[] = [];
  const raw = {
    topology: {
      peer: () => ({ descriptor: { lifecycleGeneration: 7n } })
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    setServiceIngress: (handler: typeof ingress) => {
      ingress = handler;
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'target', 3n);
  const events: string[] = [];
  let committedRoute: ServiceInstanceRouteFence | undefined;
  let completedRoute: ServiceInstanceRouteFence | undefined;
  let releaseRead!: () => void;
  const readBarrier = new Promise<void>(resolve => {
    releaseRead = resolve;
  });
  const authority: ServiceAsyncInstanceActivationAuthority = {
    read: async () => {
      events.push('read');
      await readBarrier;
      return { kind: 'missing' };
    },
    reserve: async (activation) => {
      events.push('reserve');
      assert.deepEqual(
        activation.metadataFrame,
        encodeServiceMetadataFrame(new Map([['trace', 'activation-1']]))
      );
      return {
        kind: 'reserved',
        reservation: {
          attempt: 12n,
          authorityOwnerGeneration: 12n,
          token: 'reservation-12'
        }
      };
    },
    resume: async () => assert.fail('Live activation must not resume a startup reservation'),
    commit: async (_target, _reservation, spot) => {
      events.push('commit');
      const committed = {
        kind: 'committed',
        route: {
          targetNodeRid: 'target',
          targetNodeGeneration: 3n,
          targetSpotId: 'tenant-async',
          objectGeneration: spot.ref.generation,
          ownerId: 'target',
          authorityOwnerGeneration: spot.authorityOwnerGeneration,
          leaseGeneration: 1n,
          storeVersion: '20'
        }
      } as const;
      committedRoute = committed.route;
      return committed;
    },
    complete: async (_target, route) => {
      events.push('complete');
      completedRoute = { ...route, storeVersion: '21' };
      return completedRoute;
    },
    abort: async () => {
      assert.fail('successful activation must not abort');
    }
  };
  runtime.registerAsyncInstanceActivationAuthority(authority);

  const activationMetadata = encodeServiceMetadataFrame(
    new Map([['trace', 'activation-1']])
  );
  const header = encodeInstanceSpotActivationHeader(
    {
      targetNodeRid: 'target',
      targetNodeGeneration: 3n,
      targetSpotId: 'tenant-async',
      stableType: 'TenantWorker',
      descriptorVersion: 'descriptor-5'
    },
    7n,
    'source',
    undefined,
    'send',
    { high: 7n, low: 33n },
    BigInt(Date.now() + 10_000),
    undefined,
    true
  );
  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: M6bServiceWireFlag.metadata,
    sourceRoutingId: 'source',
    parts: [
      header,
      activationMetadata,
      encodeApplicationPayload({
        packetName: 'FirstMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('first')
      })
    ]
  }), 'infrastructure');
  assert.deepEqual(events, ['read']);
  assert.equal(queued.length, 0);

  releaseRead();
  await new Promise<void>(resolve => setImmediate(resolve));
  assert.deepEqual(events, ['read', 'reserve', 'commit']);
  assert.equal(runtime.registry.spot('tenant-async')?.stableType, 'TenantWorker');
  assert.equal(queued.length, 1);
  const firstRecord = queued[0] as {
    readonly stateful?: {
      readonly applicationMetadata?: Buffer;
      readonly onTerminalCompletion?: () => Promise<void>;
    };
  };
  assert.deepEqual(firstRecord.stateful?.applicationMetadata, activationMetadata);
  assert.ok(firstRecord.stateful?.onTerminalCompletion);
  await firstRecord.stateful!.onTerminalCompletion!();
  assert.deepEqual(events, ['read', 'reserve', 'commit', 'complete']);
  if (committedRoute === undefined) throw new Error('Ready route was not committed.');
  if (completedRoute === undefined) throw new Error('Terminal route was not committed.');
  const instanceIntent = (
    runtime as unknown as {
      readonly instanceIntents: ReadonlyMap<
        string,
        { readonly route: ServiceInstanceRouteFence }
      >
    }
  ).instanceIntents.get('tenant-async');
  assert.deepEqual(instanceIntent?.route, completedRoute);
  const followingHeader = encodeInstanceSpotHeader(
    completedRoute,
    7n,
    'source',
    undefined,
    'send',
    { high: 0n, low: 0n }
  );
  assert.equal(ingress({
    command: M6bServiceWireCommand.instanceSpot,
    flags: 0,
    sourceRoutingId: 'source',
    parts: [
      followingHeader,
      encodeApplicationPayload({
        packetName: 'FollowingMessage',
        contentType: 'application/octet-stream',
        payload: Buffer.from('following')
      })
    ]
  }), 'application');
  assert.equal(queued.length, 2);
  runtime.close();
});

test('Instance application factory initializes before the first recovered handler turn', async () => {
  const events: string[] = [];
  class FirstMessageHandler implements ZLinkSpotPacketHandler<ZLinkInstanceSpot, { value: number }> {
    async handle(_spot: ZLinkInstanceSpot, message: { value: number }): Promise<void> {
      events.push(`handle:${message.value}`);
    }
  }
  class TenantInstance implements ZLinkInstanceSpot {
    declare readonly context: ZLinkInstanceSpotContext;

    configure(): void {
      events.push('configure');
      this.context.handlers.addPacket(FirstMessageHandler);
    }

    async onInitialize(): Promise<void> {
      events.push('initialize');
    }
  }
  const manager = new DefaultZLinkSpotManager({
    spotFactories: [],
    instanceSpotFactories: new Map([
      ['mesh-a', new Map([['TenantWorker', TenantInstance]])]
    ])
  });
  await manager.materializeInstance('mesh-a', 'TenantWorker', 'tenant:factory', 1n);
  const parts = encodeChannelEnvelopeParts(
    ZLinkChannelMessageKind.Command,
    'instance',
    FirstMessageHandler.name,
    { value: 7 }
  ).map(toBindingMessage);
  try {
    await manager.dispatchMeshInstance(
      'mesh-a',
      {
        ownerKind: 2,
        domain: ReadyDomain.Application,
        spotId: 'tenant:factory',
        actor: null
      },
      {
        kind: ReceiveKind.InstanceSpotActivation,
        domain: ReadyDomain.Application,
        sourceNodeRid: null,
        sourceSpotId: null,
        sourceBindingGeneration: 0n,
        sourceActor: null,
        operationId: { high: 1n, low: 1n },
        operationKind: 0,
        channelName: null,
        topic: null,
        applicationMetadata: null,
        kindData: null,
        terminalResult: 0,
        failureErrno: 0,
        parts,
        reply: () => SubmitResult.InvalidState,
        replyActorJoin: () => SubmitResult.NotSupported
      }
    );
  } finally {
    for (const part of parts) part.close();
  }
  assert.deepEqual(events, ['configure', 'initialize', 'handle:7']);
});

test('direct Spot route rematerializes an Instance Spot before dispatch', async () => {
  const events: string[] = [];
  class FirstMessageHandler implements ZLinkSpotPacketHandler<ZLinkInstanceSpot, { value: number }> {
    async handle(_spot: ZLinkInstanceSpot, message: { value: number }): Promise<void> {
      events.push(`handle:${message.value}`);
    }
  }
  class TenantInstance implements ZLinkInstanceSpot {
    declare readonly context: ZLinkInstanceSpotContext;

    configure(): void {
      events.push('configure');
      this.context.handlers.addPacket(FirstMessageHandler);
    }

    async onInitialize(): Promise<void> {
      events.push('initialize');
    }
  }
  const manager = new DefaultZLinkSpotManager({
    spotFactories: [],
    instanceSpotFactories: new Map([
      ['mesh-a', new Map([['TenantWorker', TenantInstance]])]
    ]),
    instanceSpotApplicationTargetProvider: () => ({
      stableType: 'TenantWorker',
      objectGeneration: 2n
    })
  });
  const parts = encodeChannelEnvelopeParts(
    ZLinkChannelMessageKind.Command,
    'instance',
    FirstMessageHandler.name,
    { value: 8 }
  ).map(toBindingMessage);
  try {
    await manager.dispatchMeshSpot(
      'mesh-a',
      {
        ownerKind: 2,
        domain: ReadyDomain.Application,
        spotId: 'tenant:direct-route',
        actor: null
      },
      {
        kind: ReceiveKind.SpotSend,
        domain: ReadyDomain.Application,
        sourceNodeRid: null,
        sourceSpotId: null,
        sourceBindingGeneration: 0n,
        sourceActor: null,
        operationId: { high: 1n, low: 2n },
        operationKind: 0,
        channelName: null,
        topic: null,
        applicationMetadata: null,
        kindData: null,
        terminalResult: 0,
        failureErrno: 0,
        parts,
        reply: () => SubmitResult.InvalidState,
        replyActorJoin: () => SubmitResult.NotSupported
      }
    );
  } finally {
    for (const part of parts) part.close();
  }
  assert.deepEqual(events, ['configure', 'initialize', 'handle:8']);
});

test('Instance Spot activation dispatch rematerializes a missing application before the handler turn', async () => {
  const events: string[] = [];
  class FirstMessageHandler implements ZLinkSpotPacketHandler<ZLinkInstanceSpot, { value: number }> {
    async handle(_spot: ZLinkInstanceSpot, message: { value: number }): Promise<void> {
      events.push(`handle:${message.value}`);
    }
  }
  class TenantInstance implements ZLinkInstanceSpot {
    declare readonly context: ZLinkInstanceSpotContext;

    configure(): void {
      events.push('configure');
      this.context.handlers.addPacket(FirstMessageHandler);
    }

    async onInitialize(): Promise<void> {
      events.push('initialize');
    }
  }
  const manager = new DefaultZLinkSpotManager({
    spotFactories: [],
    instanceSpotFactories: new Map([
      ['mesh-a', new Map([['TenantWorker', TenantInstance]])]
    ]),
    instanceSpotApplicationTargetProvider: () => ({
      stableType: 'TenantWorker',
      objectGeneration: 3n
    })
  });
  const parts = encodeChannelEnvelopeParts(
    ZLinkChannelMessageKind.Command,
    'instance',
    FirstMessageHandler.name,
    { value: 9 }
  ).map(toBindingMessage);
  try {
    await manager.dispatchMeshInstance(
      'mesh-a',
      {
        ownerKind: 2,
        domain: ReadyDomain.Application,
        spotId: 'tenant:stateful-route',
        actor: null
      },
      {
        kind: ReceiveKind.InstanceSpotActivation,
        domain: ReadyDomain.Application,
        sourceNodeRid: null,
        sourceSpotId: null,
        sourceBindingGeneration: 0n,
        sourceActor: null,
        operationId: { high: 1n, low: 3n },
        operationKind: 0,
        channelName: null,
        topic: null,
        applicationMetadata: null,
        kindData: null,
        terminalResult: 0,
        failureErrno: 0,
        parts,
        reply: () => SubmitResult.InvalidState,
        replyActorJoin: () => SubmitResult.NotSupported
      }
    );
  } finally {
    for (const part of parts) part.close();
  }
  assert.deepEqual(events, ['configure', 'initialize', 'handle:9']);
});

test('membership and session binding generations advance and remain scoped to their session owner', () => {
  const registry = new ServiceStatefulRegistry('node-a', 1n);
  const firstSpot = registry.createSpot('spot-a');
  const secondSpot = registry.createSpot('spot-b');
  const actor = registry.createActor('actor-a', 'Player', firstSpot.ref);
  const binding = registry.bindSession(actor.ref, 'session-a', 'node-session');
  const transition = registry.joinActor(actor.ref, secondSpot.ref);
  assert.equal(transition.previousMembershipEpoch, 1n);
  assert.equal(transition.currentMembershipEpoch, 2n);
  assert.equal(registry.binding(actor.ref)?.membershipEpoch, 2n);
  assert.equal(registry.validateBoundSession(actor.ref, binding.bindingGeneration).sessionRid, 'session-a');
  assert.throws(
    () => registry.unbindSession(actor.ref, binding.bindingGeneration + 1n),
    ServiceStaleGenerationError
  );
  registry.installSessionBinding({
    ...binding,
    sessionRid: 'session-b',
    sessionOwnerNodeRid: 'node-session-b',
    bindingGeneration: 1n
  });
  assert.throws(
    () => registry.unbindSession(
      actor.ref,
      binding.bindingGeneration,
      binding.sessionRid,
      binding.sessionOwnerNodeRid
    ),
    ServiceStaleGenerationError
  );
  assert.equal(registry.binding(actor.ref)?.sessionRid, 'session-b');
  assert.equal(registry.unbindSession(actor.ref, 1n, 'session-b', 'node-session-b'), true);
});

test('Spot and Actor turns serialize per owner while independent owners progress', async () => {
  const registry = new ServiceStatefulRegistry('node-a', 1n);
  const events: string[] = [];
  let releaseFirst!: () => void;
  const firstBarrier = new Promise<void>(resolve => {
    releaseFirst = resolve;
  });
  const first = registry.runTurn('spot:a', async () => {
    events.push('a1-start');
    await firstBarrier;
    events.push('a1-end');
  });
  const second = registry.runTurn('spot:a', () => {
    events.push('a2');
  });
  const independent = registry.runTurn('spot:b', () => {
    events.push('b1');
  });

  await independent;
  assert.deepEqual(events, ['a1-start', 'b1']);
  releaseFirst();
  await Promise.all([first, second]);
  assert.deepEqual(events, ['a1-start', 'b1', 'a1-end', 'a2']);
});

test('reply, timeout and shutdown races settle each Promise exactly once', async () => {
  const clock = new ManualClock();
  const operations = new ServiceTerminalOperationRegistry<number>(
    new OperationRegistry<number>(clock)
  );

  const replyWins = operations.reserve(10);
  assert.equal(operations.reply(replyWins.id, 7), true);
  clock.fireAll();
  assert.equal(await replyWins.promise, 7);
  assert.equal(operations.reply(replyWins.id, 8), false);

  const timeoutWins = operations.reserve(10);
  const timeoutResult = assert.rejects(timeoutWins.promise, OperationTimeoutError);
  clock.fireAll();
  await timeoutResult;
  assert.equal(operations.reply(timeoutWins.id, 9), false);

  const shutdownWins = operations.reserve(10);
  const shutdownResult = assert.rejects(shutdownWins.promise, OperationCancelledError);
  operations.close();
  clock.fireAll();
  await shutdownResult;
});

test('bound session transition wire format fences the binding generation', () => {
  const header = encodeBoundSessionBindHeader(
    23n,
    {
      actor: { nodeRid: 'node-a', actorId: 'actor-a', generation: 2n },
      targetNodeGeneration: 4n,
      authorityOwnerGeneration: 6n
    },
    'session-a',
    { state: 'tombstone', retiredGeneration: 9n }
  );
  assert.deepEqual(decodeStatefulHeader(header), {
    kind: 'boundSessionBind',
    correlation: 23n,
    actor: {
      actor: { nodeRid: 'node-a', actorId: 'actor-a', generation: 2n },
      targetNodeGeneration: 4n,
      authorityOwnerGeneration: 6n
    },
    sessionRid: 'session-a',
    binding: { state: 'tombstone', retiredGeneration: 9n }
  });
});

test('authority reconciliation exact-reads complete scans and publishes only Ready mesh-local routes', async () => {
  const readyV1 = instanceAuthoritySnapshot({
    spotId: 'tenant:42',
    meshName: 'mesh-b',
    nodeRid: 'node-b',
    storeVersion: 'store-v1',
    authorityOwnerGeneration: 7n,
    state: 'ready'
  });
  const cold = instanceAuthoritySnapshot({
    spotId: 'tenant:cold',
    meshName: 'mesh-a',
    nodeRid: 'node-a',
    storeVersion: 'store-cold',
    authorityOwnerGeneration: 3n,
    state: 'coldActivating'
  });
  const store = new ReconcileAuthorityStore([
    ['row:tenant:42', readyV1],
    ['row:tenant:cold', cold]
  ]);
  const nodeA = new RecordingAuthorityNode('mesh-a', 'node-a');
  const nodeB = new RecordingAuthorityNode('mesh-b', 'node-b');
  const changedSpotIds: string[] = [];
  const runtime = new ZLinkStatefulAuthorityRouteRuntime({
    store,
    meshNodes: new Map([
      ['mesh-a', nodeA as unknown as ZLinkBackendMeshNode],
      ['mesh-b', nodeB as unknown as ZLinkBackendMeshNode]
    ]),
    pollingIntervalMs: 60_000,
    pageSize: 1,
    reportError: (error) => {
      throw error;
    },
    onSpotRouteChanged: (spotId) => changedSpotIds.push(spotId)
  });

  await runtime.reconcile();
  assert.deepEqual(store.readKeys, []);
  assert.equal(nodeA.remembered.length, 0);
  assert.equal(nodeB.remembered.length, 1);
  assert.equal(nodeB.remembered[0]!.route.spot.spotId, 'tenant:42');
  assert.equal(nodeB.intents[0]!.route.storeVersion, 'store-v1');
  assert.deepEqual(changedSpotIds, ['tenant:42']);

  const readyV2 = instanceAuthoritySnapshot({
    spotId: 'tenant:42',
    meshName: 'mesh-b',
    nodeRid: 'node-b',
    storeVersion: 'store-v2',
    authorityOwnerGeneration: 7n,
    state: 'ready'
  });
  store.replace('row:tenant:42', readyV2);
  await runtime.reconcile();
  assert.equal(nodeB.intents.at(-1)!.route.storeVersion, 'store-v2');
  assert.deepEqual(nodeB.forgottenIntents.at(-1), {
    spotId: 'tenant:42',
    objectGeneration: 11n,
    authorityOwnerGeneration: 7n,
    storeVersion: 'store-v1'
  });
  assert.deepEqual(changedSpotIds, ['tenant:42', 'tenant:42']);

  store.scanExpired = true;
  store.replace('row:tenant:42', cold);
  const cleanupCount = nodeB.forgottenIntents.length;
  await runtime.reconcile();
  assert.equal(nodeB.forgottenIntents.length, cleanupCount);

  store.scanExpired = false;
  await runtime.reconcile();
  assert.equal(nodeB.forgottenIntents.at(-1)!.storeVersion, 'store-v2');
  assert.deepEqual(changedSpotIds, ['tenant:42', 'tenant:42', 'tenant:42']);
});

test('live Instance route fences reject an older reconcile snapshot', () => {
  const raw = {
    topology: { peer: () => undefined },
    setServiceIngress() {},
    sendService: () => true
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-a', 1n);
  const routeV1: ServiceInstanceRouteFence = {
    targetNodeRid: 'node-a',
    targetNodeGeneration: 1n,
    targetSpotId: 'tenant:race',
    objectGeneration: 11n,
    ownerId: 'owner-a',
    authorityOwnerGeneration: 7n,
    leaseGeneration: 5n,
    storeVersion: 'store-v1'
  };
  const routeV2 = { ...routeV1, storeVersion: 'store-v2' };
  const directV1 = {
    spot: { spotId: routeV1.targetSpotId, generation: routeV1.objectGeneration },
    targetNodeRid: routeV1.targetNodeRid,
    targetNodeGeneration: routeV1.targetNodeGeneration,
    authorityOwnerGeneration: routeV1.authorityOwnerGeneration,
    ownerLeaseGeneration: routeV1.leaseGeneration,
    storeVersion: routeV1.storeVersion
  };
  const directV2 = { ...directV1, storeVersion: routeV2.storeVersion };

  runtime.registerInstanceIntent('TenantWorker', routeV1, null);
  runtime.rememberSpotRoute(directV1, null);
  // The live terminal completion advances the opaque StoreVersion.
  runtime.registerInstanceIntent('TenantWorker', routeV2);
  runtime.rememberSpotRoute(directV2);
  // A reconcile started from v1 must not replace the live v2 route.
  runtime.registerInstanceIntent('TenantWorker', routeV1, routeV1);
  runtime.rememberSpotRoute(directV1, directV1);
  // Once the durable snapshot catches up, an already-current live route may
  // be accepted even though the snapshot baseline is still the older route.
  runtime.registerInstanceIntent('TenantWorker', routeV2, routeV1);
  runtime.rememberSpotRoute(directV2, directV1);

  const intents = (runtime as unknown as {
    readonly instanceIntents: Map<string, { readonly route: ServiceInstanceRouteFence }>;
  }).instanceIntents;
  const directRoutes = (runtime as unknown as {
    readonly directSpotRoutes: Map<string, { readonly storeVersion: string }>;
  }).directSpotRoutes;
  assert.equal(intents.get(routeV1.targetSpotId)?.route.storeVersion, 'store-v2');
  assert.equal(directRoutes.get(routeV1.targetSpotId)?.storeVersion, 'store-v2');
  runtime.close();
});

test('authority reconciliation refuses startup when the recovery scan expires', async () => {
  const store = new ReconcileAuthorityStore([]);
  store.scanExpired = true;
  const runtime = new ZLinkStatefulAuthorityRouteRuntime({
    store,
    meshNodes: new Map(),
    pollingIntervalMs: 60_000,
    pageSize: 1,
    reportError: () => undefined
  });

  await assert.rejects(
    runtime.start(),
    /initial authority recovery scan expired/
  );
});

test('authority reconciliation restores the durable Instance inbox before startup returns', async () => {
  const recoveryEnvelope = encodeInstanceActivationRecoveryEnvelope({
    target: {
      targetSpotId: 'tenant:recover',
      stableType: 'TenantWorker',
      targetNodeRid: 'node-a',
      targetNodeGeneration: 1n,
      descriptorVersion: '1'
    },
    targetMeshName: 'mesh-a',
    sourceNodeRid: 'source-a',
    sourceNodeGeneration: 2n,
    operationKind: 'send',
    operation: { high: 1n, low: 7n },
    deadlineUnixMs: 99_999n,
    applicationPayloadFrame: encodeApplicationPayload({
      packetName: 'TenantRequest',
      contentType: 'application/octet-stream',
      payload: Buffer.from('recover')
    })
  });
  const snapshot = instanceAuthoritySnapshot({
    spotId: 'tenant:recover',
    meshName: 'mesh-a',
    nodeRid: 'node-a',
    storeVersion: 'store-recovery',
    authorityOwnerGeneration: 3n,
    state: 'ready',
    activationRecovery: {
      reference: 'relocation:recover',
      sha256: createHash('sha256').update(recoveryEnvelope).digest(),
      encodedSize: recoveryEnvelope.byteLength,
      inboxSequence: 1n,
      replayCursor: 0n
    }
  });
  const node = new RecordingAuthorityNode('mesh-a', 'node-a');
  const runtime = new ZLinkStatefulAuthorityRouteRuntime({
    store: new ReconcileAuthorityStore([['row:tenant:recover', snapshot]]),
    relocationStore: {
      put: async () => assert.fail('Recovery must not write a second root.'),
      read: async () => foundBlob(recoveryEnvelope),
      renew: async () => missingRenewal(),
      delete: async () => assert.fail('The authority adapter owns root deletion.')
    },
    meshNodes: new Map([['mesh-a', node as unknown as ZLinkBackendMeshNode]]),
    pollingIntervalMs: 60_000,
    pageSize: 10,
    reportError: (error) => {
      throw error;
    }
  });

  await runtime.reconcile(undefined, true);
  assert.equal(node.recovered.length, 1);
  assert.equal(node.recovered[0]!.envelope.target.targetSpotId, 'tenant:recover');
  assert.equal(node.recovered[0]!.route.storeVersion, 'store-recovery');

  const staleDescriptorNode = new RecordingAuthorityNode(
    'mesh-a',
    'node-a',
    undefined,
    2n
  );
  const staleDescriptorRuntime = new ZLinkStatefulAuthorityRouteRuntime({
    store: new ReconcileAuthorityStore([['row:tenant:recover', snapshot]]),
    relocationStore: {
      put: async () => assert.fail('Recovery must not write a second root.'),
      read: async () => foundBlob(recoveryEnvelope),
      renew: async () => missingRenewal(),
      delete: async () => assert.fail('The authority adapter owns root deletion.')
    },
    meshNodes: new Map([
      ['mesh-a', staleDescriptorNode as unknown as ZLinkBackendMeshNode]
    ]),
    pollingIntervalMs: 60_000,
    pageSize: 10,
    reportError: () => undefined
  });
  await staleDescriptorRuntime.reconcile(undefined, true);
  assert.equal(staleDescriptorNode.recovered.length, 0);
  assert.equal(staleDescriptorNode.forgotten.length, 1);
});

test('authority reconciliation exact-reads Ready Instance activation recovery candidates', async () => {
  const recovery = {
    reference: 'relocation:stale-ready',
    sha256: Buffer.alloc(32, 0x5a),
    encodedSize: 256,
    inboxSequence: 2n,
    replayCursor: 1n
  };
  const scanSnapshot = instanceAuthoritySnapshot({
    spotId: 'tenant:stale-ready',
    meshName: 'mesh-a',
    nodeRid: 'node-a',
    storeVersion: 'store-scan',
    authorityOwnerGeneration: 3n,
    state: 'ready',
    activationRecovery: recovery
  });
  const exactSnapshot = instanceAuthoritySnapshot({
    spotId: 'tenant:stale-ready',
    meshName: 'mesh-a',
    nodeRid: 'node-a',
    storeVersion: 'store-exact',
    authorityOwnerGeneration: 4n,
    state: 'ready'
  });
  const store = new ReconcileAuthorityStore([
    ['row:tenant:stale-ready', exactSnapshot]
  ]);
  store.scanOverrides.set('row:tenant:stale-ready', scanSnapshot);
  const node = new RecordingAuthorityNode('mesh-a', 'node-a');
  const runtime = new ZLinkStatefulAuthorityRouteRuntime({
    store,
    relocationStore: {
      put: async () => assert.fail('The exact Ready snapshot must not recover stale data.'),
      read: async () => assert.fail('The exact Ready snapshot has no recovery pointer.'),
      renew: async () => missingRenewal(),
      delete: async () => assert.fail('The exact Ready snapshot has no recovery pointer.')
    },
    meshNodes: new Map([['mesh-a', node as unknown as ZLinkBackendMeshNode]]),
    pollingIntervalMs: 60_000,
    pageSize: 10,
    reportError: (error) => {
      throw error;
    }
  });

  await runtime.reconcile(undefined, true);

  assert.deepEqual(store.readKeys, ['row:tenant:stale-ready']);
  assert.equal(node.recovered.length, 0);
  assert.equal(node.intents.length, 1);
  assert.equal(node.intents[0]!.route.storeVersion, 'store-exact');
});

test('authority reconciliation resumes an exact Pending Instance reservation', async () => {
  const recoveryEnvelope = encodeInstanceActivationRecoveryEnvelope({
    target: {
      targetSpotId: 'tenant:pending',
      stableType: 'TenantWorker',
      targetNodeRid: 'node-a',
      targetNodeGeneration: 1n,
      descriptorVersion: '1'
    },
    targetMeshName: 'mesh-a',
    sourceNodeRid: 'source-a',
    sourceNodeGeneration: 2n,
    operationKind: 'send',
    operation: { high: 1n, low: 8n },
    deadlineUnixMs: 99_999n,
    applicationPayloadFrame: encodeApplicationPayload({
      packetName: 'TenantRequest',
      contentType: 'application/octet-stream',
      payload: Buffer.from('pending')
    })
  });
  const requestSha256 = createHash('sha256').update(recoveryEnvelope).digest();
  const snapshot = instanceAuthoritySnapshot({
    spotId: 'tenant:pending',
    meshName: 'mesh-a',
    nodeRid: 'node-a',
    storeVersion: 'store-pending',
    authorityOwnerGeneration: 3n,
    state: 'coldActivating',
    pendingCreation: {
      reservationId: 'reservation-pending',
      requestContentReference: 'relocation:pending',
      requestSha256,
      requestEncodedSize: BigInt(recoveryEnvelope.byteLength)
    }
  });
  const node = new RecordingAuthorityNode('mesh-a', 'node-a');
  const store = new ReconcileAuthorityStore([['row:tenant:pending', snapshot]]);
  const runtime = new ZLinkStatefulAuthorityRouteRuntime({
    store,
    relocationStore: {
      put: async () => assert.fail('Recovery must not write a second root.'),
      read: async () => foundBlob(recoveryEnvelope),
      renew: async () => missingRenewal(),
      delete: async () => assert.fail('The authority adapter owns root deletion.')
    },
    meshNodes: new Map([['mesh-a', node as unknown as ZLinkBackendMeshNode]]),
    pollingIntervalMs: 60_000,
    pageSize: 10,
    reportError: (error) => {
      throw error;
    }
  });

  await runtime.reconcile(undefined, true);
  assert.deepEqual(store.readKeys, ['row:tenant:pending']);
  assert.equal(node.recoveredPending.length, 1);
  assert.equal(
    node.recoveredPending[0]!.pending.reservationId,
    'reservation-pending'
  );
  assert.equal(
    node.recoveredPending[0]!.envelope.target.targetSpotId,
    'tenant:pending'
  );
});

test('production Instance authority adapter writes schema ColdActivating then Ready payloads', async () => {
  const store = new ZLinkInMemoryAuthorityStore({ isTargetLive: () => true });
  const compareExchangeAuthority = store.compareExchangeAuthority.bind(store);
  let preserveAttempts = 0;
  store.compareExchangeAuthority = async (key, expected, mutation, signal) => {
    preserveAttempts += 1;
    if (preserveAttempts === 2) {
      throw new Error('simulated crash before recovery pointer release');
    }
    return await compareExchangeAuthority(key, expected, mutation, signal);
  };
  let recordedRequestReference: string | undefined;
  const reserve = store.reserve.bind(store);
  store.reserve = async (request, signal) => {
    recordedRequestReference = request.intent.requestContentReference;
    return await reserve(request, signal);
  };
  let storedRequest: Uint8Array | undefined;
  let requestReference: ZLinkBlobReference | undefined;
  const relocationStore: ZLinkRelocationStore = {
    put: async (reference, payload) => {
      requestReference = reference;
      storedRequest = Buffer.from(payload);
      const now = new Date();
      return {
        kind: 'stored',
        expiresAt: new Date(now.getTime() + 60_000),
        storeNow: now
      };
    },
    read: async () => storedRequest === undefined
      ? missingBlob()
      : foundBlob(storedRequest),
    renew: async () => missingRenewal(),
    delete: async () => {
      throw new Error('simulated orphan cleanup failure');
    }
  };
  const authority = new ZLinkInstanceActivationAuthority({
    store,
    relocationStore,
    meshName: 'mesh-a',
    owner: () => ({ ownerId: 'owner-a', leaseGeneration: 5n })
  });
  const target = {
    targetNodeRid: 'node-a',
    targetNodeGeneration: 1n,
    targetSpotId: 'tenant:42',
    stableType: 'TenantWorker',
    descriptorVersion: 'descriptor-v1'
  };
  assert.deepEqual(await authority.read(target), { kind: 'missing' });
  const reserved = await authority.reserve({
    target,
    sourceNodeRid: 'source-a',
    sourceNodeGeneration: 2n,
    operationKind: 'send',
    operation: { high: 1n, low: 2n },
    deadlineUnixMs: 99_999n,
    applicationPayloadFrame: encodeApplicationPayload({
      packetName: 'TenantRequest',
      contentType: 'application/octet-stream',
      payload: Buffer.from('create')
    })
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') throw new Error('Instance reservation failed.');
  const creating = await store.readAuthority({
    value: encodeAuthorityKey('instance_spot', 'tenant:42').value
  } as ZLinkAuthorityKey);
  assert.equal(creating.kind, 'snapshot');
  if (creating.kind !== 'snapshot') throw new Error('Creating authority is missing.');
  assert.equal(decodeServiceReadySpotAuthority(creating.payload), undefined);
  assert.deepEqual(await authority.read(target), {
    kind: 'creating',
    objectGeneration: creating.objectGeneration,
    authorityOwnerGeneration: creating.authorityOwnerGeneration
  });
  assert.ok(storedRequest !== undefined);
  assert.equal(recordedRequestReference, requestReference?.value);
  assert.equal(
    decodeInstanceActivationRecoveryEnvelope(storedRequest!).targetMeshName,
    'mesh-a'
  );

  let readyCallbackRoute: ServiceInstanceRouteFence | undefined;
  let commitPromiseResolved = false;
  const resumedAuthority = new ZLinkInstanceActivationAuthority({
    store,
    relocationStore,
    meshName: 'mesh-a',
    owner: () => ({ ownerId: 'owner-a', leaseGeneration: 5n }),
    onReady: (_readyTarget, route) => {
      // The route projection must be updated before commit resolves so the
      // first application admission cannot observe a durable Ready without
      // its local target intent.
      assert.equal(commitPromiseResolved, false);
      readyCallbackRoute = route;
    }
  });
  const projection = creating.pendingCreation;
  if (projection === undefined) throw new Error('Pending creation projection is missing.');
  const resumed = await resumedAuthority.resume(target, {
    reservationId: projection.reservationId,
    storeVersion: creating.storeVersion.value,
    objectGeneration: creating.objectGeneration,
    authorityOwnerGeneration: creating.authorityOwnerGeneration,
    ownerId: creating.ownerId,
    ownerLeaseGeneration: creating.ownerLeaseGeneration,
    meshName: creating.allocation.descriptor.meshName,
    nodeRid: String(creating.allocation.descriptor.rid),
    nodeGeneration: creating.allocation.descriptorLifecycleGeneration,
    requestReference: projection.requestContentReference,
    requestSha256: projection.requestSha256,
    requestEncodedSize: projection.requestEncodedSize
  });
  assert.deepEqual(resumed, reserved.reservation);
  const commitPromise = resumedAuthority.commit(
    target,
    resumed,
    {
      kind: 'instance',
      stableType: 'TenantWorker',
      ref: { spotId: 'tenant:42', generation: reserved.reservation.attempt },
      authorityOwnerGeneration: creating.authorityOwnerGeneration
    } as never
  );
  void commitPromise.then(() => {
    commitPromiseResolved = true;
  });
  const committed = await commitPromise;
  assert.equal(committed.kind, 'committed');
  assert.deepEqual(readyCallbackRoute, committed.route);
  const committedSnapshot = await store.readAuthority({
    value: encodeAuthorityKey('instance_spot', 'tenant:42').value
  } as ZLinkAuthorityKey);
  assert.equal(committedSnapshot.kind, 'snapshot');
  if (committedSnapshot.kind !== 'snapshot') throw new Error('Ready authority is missing.');
  assert.equal(
    decodeServiceReadySpotAuthority(committedSnapshot.payload)?.activationRecovery?.reference,
    requestReference?.value
  );
  const ready = await resumedAuthority.read(target);
  assert.equal(ready.kind, 'ready');
  if (ready.kind === 'ready') {
    assert.equal(ready.route.targetSpotId, 'tenant:42');
    assert.equal(ready.route.storeVersion, committed.route.storeVersion);
  }
  await assert.rejects(
    resumedAuthority.complete(target, committed.route),
    /simulated crash before recovery pointer release/
  );
  const terminalRecorded = await store.readAuthority({
    value: encodeAuthorityKey('instance_spot', 'tenant:42').value
  } as ZLinkAuthorityKey);
  assert.equal(terminalRecorded.kind, 'snapshot');
  if (terminalRecorded.kind !== 'snapshot') {
    throw new Error('Terminal completion authority is missing.');
  }
  assert.deepEqual(
    decodeServiceReadySpotAuthority(terminalRecorded.payload)?.activationRecovery,
    {
      reference: requestReference?.value,
      sha256: createHash('sha256').update(storedRequest!).digest(),
      encodedSize: storedRequest!.byteLength,
      inboxSequence: 1n,
      replayCursor: 1n
    }
  );
  assert.ok(storedRequest !== undefined);

  // A restarted authority observes the durable cursor and performs only the
  // pointer release; it must not need to repeat the terminal handler.
  store.compareExchangeAuthority = compareExchangeAuthority;
  const restartedAuthority = new ZLinkInstanceActivationAuthority({
    store,
    relocationStore,
    meshName: 'mesh-a',
    owner: () => ({ ownerId: 'owner-a', leaseGeneration: 5n })
  });
  let recoveryReleaseCount = 0;
  const recoveryNode = new RecordingAuthorityNode(
    'mesh-a',
    'node-a',
    async (recoveryTarget, route) => {
      recoveryReleaseCount += 1;
      return await restartedAuthority.complete(recoveryTarget, route);
    }
  );
  const recoveryRuntime = new ZLinkStatefulAuthorityRouteRuntime({
    store,
    relocationStore,
    meshNodes: new Map([
      ['mesh-a', recoveryNode as unknown as ZLinkBackendMeshNode]
    ]),
    pollingIntervalMs: 60_000,
    pageSize: 10,
    reportError: (error) => {
      throw error;
    }
  });
  await recoveryRuntime.reconcile(undefined, true);
  assert.equal(recoveryReleaseCount, 1);
  assert.equal(recoveryNode.recovered.length, 0);
  const releasedSnapshot = await store.readAuthority({
    value: encodeAuthorityKey('instance_spot', 'tenant:42').value
  } as ZLinkAuthorityKey);
  assert.equal(releasedSnapshot.kind, 'snapshot');
  if (releasedSnapshot.kind !== 'snapshot') throw new Error('Released authority is missing.');
  assert.notEqual(releasedSnapshot.storeVersion.value, committed.route.storeVersion);
  assert.equal(
    recoveryNode.remembered.at(-1)!.route.storeVersion,
    releasedSnapshot.storeVersion.value
  );
  assert.equal(
    decodeServiceReadySpotAuthority(releasedSnapshot.payload)?.activationRecovery,
    undefined
  );
  assert.ok(storedRequest !== undefined);
});

test('production Instance Ready commit Store rejection is exposed as RequestFailed with the original cause', async () => {
  const store = new ZLinkInMemoryAuthorityStore({ isTargetLive: () => true });
  const requestPayloads = new Map<string, Uint8Array>();
  const relocationStore: ZLinkRelocationStore = {
    put: async (reference, payload) => {
      requestPayloads.set(reference.value, Buffer.from(payload));
      const now = new Date();
      return {
        kind: 'stored',
        expiresAt: new Date(now.getTime() + 60_000),
        storeNow: now
      };
    },
    read: async (reference) => {
      const payload = requestPayloads.get(reference.value);
      return payload === undefined
        ? missingBlob()
        : foundBlob(payload);
    },
    renew: async () => missingRenewal(),
    delete: async (reference) => {
      requestPayloads.delete(reference.value);
    }
  };
  const authority = new ZLinkInstanceActivationAuthority({
    store,
    relocationStore,
    meshName: 'mesh-a',
    owner: () => ({ ownerId: 'owner-a', leaseGeneration: 5n })
  });
  const target = {
    targetNodeRid: 'node-a',
    targetNodeGeneration: 1n,
    targetSpotId: 'tenant:commit-fault',
    stableType: 'TenantWorker',
    descriptorVersion: 'descriptor-v1'
  };
  const reserved = await authority.reserve({
    target,
    sourceNodeRid: 'source-a',
    sourceNodeGeneration: 2n,
    operationKind: 'send',
    operation: { high: 1n, low: 2n },
    deadlineUnixMs: 99_999n,
    applicationPayloadFrame: encodeApplicationPayload({
      packetName: 'TenantRequest',
      contentType: 'application/octet-stream',
      payload: Buffer.from('create')
    })
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') return;
  const creating = await store.readAuthority({
    value: encodeAuthorityKey('instance_spot', target.targetSpotId).value
  } as ZLinkAuthorityKey);
  assert.equal(creating.kind, 'snapshot');
  if (creating.kind !== 'snapshot') return;
  const commitFault = new Error('Instance commit unavailable');
  store.commit = async () => {
    throw commitFault;
  };
  await assert.rejects(
    () => authority.commit(target, reserved.reservation, {
      kind: 'instance',
      stableType: target.stableType,
      ref: {
        spotId: target.targetSpotId,
        generation: reserved.reservation.attempt
      },
      authorityOwnerGeneration: creating.authorityOwnerGeneration
    } as never),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.InternalFailure
      && error.cause === commitFault
  );
});

test('concurrent Instance activation CAS loser joins Ready and returns the winner route', async () => {
  const store = new ZLinkInMemoryAuthorityStore({ isTargetLive: () => true });
  const roots = new Map<string, Buffer>();
  const relocationStore: ZLinkRelocationStore = {
    put: async (reference, payload) => {
      const bytes = Buffer.from(payload);
      roots.set(reference.value, bytes);
      const now = new Date();
      return {
        kind: 'stored',
        expiresAt: new Date(now.getTime() + 60_000),
        storeNow: now
      };
    },
    read: async (reference) => {
      const payload = roots.get(reference.value);
      return payload === undefined
        ? missingBlob()
        : foundBlob(payload);
    },
    renew: async () => missingRenewal(),
    delete: async () => {
      throw new Error('simulated CAS-loser orphan cleanup failure');
    }
  };
  const winnerAuthority = new ZLinkInstanceActivationAuthority({
    store,
    relocationStore,
    meshName: 'mesh-a',
    owner: () => ({ ownerId: 'owner-a', leaseGeneration: 1n })
  });
  const loserAuthority = new ZLinkInstanceActivationAuthority({
    store,
    relocationStore,
    meshName: 'mesh-a',
    owner: () => ({ ownerId: 'owner-b', leaseGeneration: 1n })
  });
  const winnerTarget = {
    targetNodeRid: 'winner-node',
    targetNodeGeneration: 3n,
    targetSpotId: 'tenant:concurrent',
    stableType: 'TenantWorker',
    descriptorVersion: 'winner-descriptor'
  };
  const loserTarget = {
    ...winnerTarget,
    targetNodeRid: 'loser-node',
    targetNodeGeneration: 4n,
    descriptorVersion: 'loser-descriptor'
  };
  const activation = {
    sourceNodeRid: 'source',
    sourceNodeGeneration: 7n,
    operationKind: 'send' as const,
    operation: { high: 9n, low: 1n },
    deadlineUnixMs: BigInt(Date.now() + 2_000),
    applicationPayloadFrame: encodeApplicationPayload({
      packetName: 'FirstMessage',
      contentType: 'application/octet-stream',
      payload: Buffer.from('concurrent')
    })
  };
  const winner = await winnerAuthority.reserve({
    ...activation,
    target: winnerTarget
  });
  assert.equal(winner.kind, 'reserved');
  if (winner.kind !== 'reserved') throw new Error('Winner did not reserve activation.');

  const loser = loserAuthority.reserve({
    ...activation,
    target: loserTarget
  });
  await new Promise<void>(resolve => setImmediate(resolve));
  const creating = await store.readAuthority(
    encodeAuthorityKey('instance_spot', winnerTarget.targetSpotId)
  );
  assert.equal(creating.kind, 'snapshot');
  if (creating.kind !== 'snapshot') throw new Error('Winner reservation is missing.');
  const committed = await winnerAuthority.commit(
    winnerTarget,
    winner.reservation,
    {
      kind: 'instance',
      stableType: winnerTarget.stableType,
      ref: {
        spotId: winnerTarget.targetSpotId,
        generation: winner.reservation.attempt
      },
      authorityOwnerGeneration: creating.authorityOwnerGeneration
    } as never
  );
  assert.equal(committed.kind, 'committed');

  const joined = await loser;
  assert.equal(joined.kind, 'ready');
  if (joined.kind !== 'ready') throw new Error('CAS loser did not join Ready.');
  assert.equal(joined.route.targetNodeRid, winnerTarget.targetNodeRid);
  assert.equal(joined.route.targetNodeGeneration, winnerTarget.targetNodeGeneration);
  assert.equal(joined.route.storeVersion, committed.route.storeVersion);
  assert.ok(
    new ServiceInstanceActivationRedirectError(joined.route)
      instanceof ServiceInstanceActivationRedirectError
  );
  assert.equal(roots.size, 2);
});

test('raw backend dispatches Spot requests and Actor sends through M6B owners', async () => {
  const nonce = `${process.pid}-${Date.now()}`;
  const endpoint = `ipc:///tmp/zlink-m6b-node-${nonce}.sock`;
  const backend = new ZLinkNodeRawMeshBackend('m6b-mesh', 'm6b-node');
  backend.setBind(endpoint);
  backend.start();
  const instanceRoute = {
    targetNodeRid: 'm6b-node',
    targetNodeGeneration: backend.status().lifecycleGeneration,
    targetSpotId: 'tenant:42',
    objectGeneration: 1n,
    ownerId: 'owner-a',
    authorityOwnerGeneration: 1n,
    leaseGeneration: 1n,
    storeVersion: 'store-v1'
  };
  const authorityRoutes = new ZLinkStatefulAuthorityRouteRuntime({
    store: singleAuthorityStore(
      'canonical-authority:tenant:42',
      {
        kind: 'snapshot',
        storeVersion: { value: instanceRoute.storeVersion } as ZLinkAuthoritySnapshot['storeVersion'],
        payload: encodeServiceInstanceAuthorityPayload({
          state: 'ready',
          stableType: 'TenantWorker',
          spotId: instanceRoute.targetSpotId,
          ownerId: instanceRoute.ownerId,
          ownerLeaseGeneration: instanceRoute.leaseGeneration,
          ownerMeshName: 'm6b-mesh',
          ownerNodeRid: instanceRoute.targetNodeRid,
          ownerNodeGeneration: instanceRoute.targetNodeGeneration
        }),
        objectGeneration: instanceRoute.objectGeneration,
        authorityOwnerGeneration: instanceRoute.authorityOwnerGeneration,
        ownerId: instanceRoute.ownerId,
        ownerLeaseGeneration: instanceRoute.leaseGeneration,
        allocation: {
          state: 'active',
          objectKind: 'instance_spot',
          stableType: 'TenantWorker',
          descriptor: {
            meshName: 'm6b-mesh',
            rid: instanceRoute.targetNodeRid
          },
          descriptorLifecycleGeneration: instanceRoute.targetNodeGeneration,
          capacity: {
            actors: 0,
            spots: 1,
            spotType: {
              objectKind: 'instance_spot',
              stableType: 'TenantWorker',
              count: 1
            }
          }
        },
        storeNow: new Date()
      }
    ),
    meshNodes: new Map([['m6b-mesh', backend]]),
    pollingIntervalMs: 60_000,
    pageSize: 100,
    reportError: (error) => {
      throw error;
    }
  });
  await authorityRoutes.start();
  try {
    const targetSpot = backend.getOrCreateSpot('spot-target').spot;
    const sourceSpot = backend.entrySpot();
    const spotRoute = {
      spot: {
        spotId: 'spot-target',
        generation: targetSpot.status().lifecycleGeneration
      },
      targetNodeRid: 'm6b-node',
      targetNodeGeneration: backend.status().lifecycleGeneration,
      authorityOwnerGeneration: targetSpot.status().lifecycleGeneration,
      ownerLeaseGeneration: 1n,
      storeVersion: 'spot-store-v1'
    };
    backend.rememberSpotRoute(spotRoute);
    const operation = sourceSpot.requestToSpot(
      'm6b-node',
      'spot-target',
      targetSpot.status().lifecycleGeneration,
      Buffer.from('spot-request'),
      { timeoutMs: 2_000, routeFence: spotRoute }
    );
    const receivedSpot = await drainOne(backend, ReadyDomain.Application);
    assert.equal(receivedSpot.kind, ReceiveKind.SpotRequest);
    assert.equal(String(receivedSpot.sourceSpotId), 'm6b-node');
    assert.equal(receivedSpot.parts[0]!.toBytes().toString(), 'spot-request');
    assert.equal(receivedSpot.reply(Buffer.from('spot-reply')), SubmitResult.Ok);
    closeParts(receivedSpot);

    const completion = await drainOne(backend, ReadyDomain.Infrastructure);
    assert.deepEqual(completion.operationId, operation);
    assert.equal(completion.terminalResult, RequestResult.Ok);
    assert.equal(completion.parts[0]!.toBytes().toString(), 'spot-reply');
    closeParts(completion);

    const actor = backend.createActor('actor-target');
    assert.equal(backend.sendToActor(actor, Buffer.from('actor-send')), SubmitResult.Ok);
    const receivedActor = await drainOne(backend, ReadyDomain.Application);
    assert.equal(receivedActor.kind, ReceiveKind.ActorSend);
    assert.equal(receivedActor.kindData, null);
    assert.equal(receivedActor.parts[0]!.toBytes().toString(), 'actor-send');
    closeParts(receivedActor);

    const instanceOperation = backend.requestInstanceSpot(
      instanceRoute,
      Buffer.from('instance-request'),
      2_000,
      'm6b-node'
    );
    const instance = await drainOne(backend, ReadyDomain.Application);
    assert.equal(instance.kind, ReceiveKind.InstanceSpotActivation);
    assert.equal(instance.parts[0]!.toBytes().toString(), 'instance-request');
    assert.equal(instance.reply(Buffer.from('instance-reply')), SubmitResult.Ok);
    closeParts(instance);
    const instanceCompletion = await drainOne(backend, ReadyDomain.Infrastructure);
    assert.deepEqual(instanceCompletion.operationId, instanceOperation);
    assert.equal(instanceCompletion.parts[0]!.toBytes().toString(), 'instance-reply');
    closeParts(instanceCompletion);

    const delivered: Buffer[] = [];
    const streamState = { disconnected: false };
    const sessionService = backend.createStreamSessionService(createFakeStream(delivered, streamState));
    sessionService.start();
    const bindOperation = sessionService.bindActor('session-a', actor, 2_000);
    const bindCompletion = await drainOne(backend, ReadyDomain.Infrastructure);
    assert.deepEqual(bindCompletion.operationId, bindOperation);
    assert.equal(bindCompletion.terminalResult, RequestResult.Ok);
    const binding = sessionService.bindings('session-a')[0]!;
    assert.equal(
      backend.sendActorBoundSession(actor, binding.bindingGeneration, Buffer.from('session-message')),
      SubmitResult.Ok
    );
    assert.deepEqual(delivered.map(value => value.toString()), ['session-message']);
    streamState.disconnected = true;
    assert.doesNotThrow(() => {
      assert.equal(
        backend.sendActorBoundSession(actor, binding.bindingGeneration, Buffer.from('late-session-message')),
        SubmitResult.InvalidState
      );
    });
    const unbindOperation = sessionService.unbindActor(
      'session-a',
      actor,
      binding.bindingGeneration,
      2_000
    );
    const unbindCompletion = await drainOne(backend, ReadyDomain.Infrastructure);
    assert.deepEqual(unbindCompletion.operationId, unbindOperation);
    assert.equal(unbindCompletion.terminalResult, RequestResult.Ok);
    assert.equal(sessionService.bindings('session-a').length, 0);
    sessionService.close();

    targetSpot.setSubscription('events', 'room-*');
    const publisher = backend.createPublisher();
    publisher.publish('events', 'room-42', Buffer.from('multicast'));
    const multicast = await drainOne(backend, ReadyDomain.Application);
    assert.equal(multicast.kind, ReceiveKind.SpotMulticast);
    assert.equal(multicast.channelName, 'events');
    assert.equal(multicast.topic, 'room-42');
    assert.equal(multicast.parts[0]!.toBytes().toString(), 'multicast');
    closeParts(multicast);
    publisher.close();

    const staleGeneration = targetSpot.status().lifecycleGeneration;
    targetSpot.close();
    const staleOperation = sourceSpot.requestToSpot(
      'm6b-node',
      'spot-target',
      staleGeneration,
      Buffer.from('stale'),
      { timeoutMs: 2_000, routeFence: spotRoute }
    );
    const staleCompletion = await drainOne(backend, ReadyDomain.Infrastructure);
    assert.deepEqual(staleCompletion.operationId, staleOperation);
    assert.equal(staleCompletion.terminalResult, RequestResult.NotFound);
    assert.equal(staleCompletion.failureErrno, 21);
  } finally {
    await authorityRoutes.stop();
    backend.close();
  }
});

test('public SpotId call reaches production host Missing Instance placement without raw runtime access', async () => {
  const submissions: Array<{
    readonly target: {
      readonly targetNodeRid: string;
      readonly targetNodeGeneration: bigint;
      readonly targetSpotId: string;
      readonly stableType: string;
      readonly descriptorVersion: string;
    };
    readonly deadline: bigint;
    readonly metadata?: ReadonlyMap<string, string>;
  }> = [];
  let requested = false;
  const node = {
    selectObjectPlacement(stableType: string) {
      assert.equal(stableType, 'chat-room');
      return {
        kind: 'selected',
        target: {
          targetNodeRid: 'node-b',
          targetNodeGeneration: 7n,
          descriptorVersion: '11'
        }
      };
    },
    sendToMissingInstanceSpot(
      target: (typeof submissions)[number]['target'],
      _parts: unknown,
      deadline: bigint,
      _source?: string,
      metadata?: ReadonlyMap<string, string>
    ) {
      submissions.push({ target, deadline, metadata });
      return SubmitResult.Ok;
    },
    requestToMissingInstanceSpot() {
      requested = true;
      return { high: 9n, low: 1n };
    }
  } as unknown as ZLinkBackendMeshNode;
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
          'missing'
        );
      }
    }),
    routed: {
      async sendToSpot() {
        throw new Error('Ready route must not be used for Missing authority.');
      },
      async requestToSpot() {
        throw new Error('request is not used by this test.');
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => node,
    completions: () => ({
      async wait() {
        return {
          terminalResult: 0,
          failureErrno: 0,
          operationKind: 39,
          kindData: null,
          parts: encodeChannelReplyParts({
            formatMarker: 0xf2,
            kind: ZLinkChannelMessageKind.Request,
            channelName: 'mesh',
            messageName: 'Ping',
            contentType: 'application/json',
            correlationId: '1',
            deadline: null,
            topic: null,
            metadata: {}
          }, 'ready-reply').map(part =>
            part instanceof Message ? part : toBindingMessage(part)
          )
        };
      }
    }) as never,
    defaultRequestTimeoutMs: 5_000
  });
  const outbound = new DefaultZLinkSpotOutbound(
    new ZLinkSpotSerialExecutor(),
    undefined,
    undefined,
    undefined,
    undefined,
    undefined,
    undefined,
    'mesh',
    undefined,
    address
  );

  class Notice {
    readonly text = 'hello';
  }
  await outbound.sendToSpot('instance-42', new Notice())
    .metadata('trace', 'abc')
    .instanceSpot('chat-room')
    .inMesh('mesh')
    .submit();

  assert.equal(submissions.length, 1);
  assert.deepEqual(submissions[0]?.target, {
    targetNodeRid: 'node-b',
    targetNodeGeneration: 7n,
    descriptorVersion: '11',
    targetSpotId: 'instance-42',
    stableType: 'chat-room'
  });
  assert.equal(submissions[0]?.metadata?.get('trace'), 'abc');
  assert.ok((submissions[0]?.deadline ?? 0n) > BigInt(Date.now()));

  await outbound.sendToSpot('instance-absent-metadata', new Notice())
    .instanceSpot('chat-room')
    .inMesh('mesh')
    .submit();
  await outbound.sendToSpot('instance-empty-metadata', new Notice())
    .metadata(ZLinkMessageMetadataEmpty)
    .instanceSpot('chat-room')
    .inMesh('mesh')
    .submit();
  assert.equal(submissions[1]?.metadata, undefined);
  assert.equal(submissions[2]?.metadata?.size, 0);

  class Ping {}
  const reply = await outbound.requestToSpot('instance-43', new Ping())
    .instanceSpot('chat-room')
    .inMesh('mesh')
    .timeout(250)
    .submit<string>();
  assert.equal(reply, 'ready-reply');
  assert.equal(requested, true);
});

test('Missing Instance distinguishes unsupported types from exhausted placement capacity', async () => {
  for (const mode of ['zeroTypes', 'noEligible'] as const) {
    const address = new ZLinkHostSpotAddressTransport({
      resolver: () => ({
        async resolve() {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
            'missing'
          );
        }
      }),
      routed: {
        async sendToSpot() {
          throw new Error('Ready route must not be used.');
        },
        async requestToSpot() {
          throw new Error('Ready route must not be used.');
        }
      },
      meshNames: () => ['mesh'],
      isMeshConfigured: () => true,
      meshNode: () => ({
            instanceSpotPlacementTypes() {
              return mode === 'zeroTypes' ? [] : ['room'];
            },
            selectObjectPlacement() {
              return { kind: 'capacity' };
            }
          } as never),
      completions: () => undefined,
      defaultRequestTimeoutMs: 100
    });
    const call = {
      instanceSpot: true,
      initialMeshName: 'mesh',
      metadata: new Map<string, string>()
    };
    if (mode === 'zeroTypes') {
      assert.deepEqual(
        await address.sendToSpotAddress('missing-room', { hello: true }, call),
        { status: ZLinkSubmitStatus.TargetNotFound }
      );
      await assert.rejects(
        () => address.requestToSpotAddress('missing-room', { hello: true }, call),
        (error: unknown) => error instanceof ZLinkFrameworkException
          && error.kind === ZLinkFrameworkErrorKind.NotFound
      );
    } else {
      await assert.rejects(
        () => address.sendToSpotAddress('missing-room', { hello: true }, call),
        (error: unknown) => error instanceof ZLinkFrameworkException
          && error.kind === ZLinkFrameworkErrorKind.CapacityExceeded
      );
      await assert.rejects(
        () => address.requestToSpotAddress('missing-room', { hello: true }, call),
        (error: unknown) => error instanceof ZLinkFrameworkException
          && error.kind === ZLinkFrameworkErrorKind.CapacityExceeded
      );
    }
  }
});

test('Missing Instance placement capacity fails without polling or retaining a call timer', async () => {
  let selectionAttempts = 0;
  const node = {
    instanceSpotPlacementTypes() {
      return ['chat-room'];
    },
    selectObjectPlacement(stableType: string) {
      assert.equal(stableType, 'chat-room');
      selectionAttempts += 1;
      return {
        kind: 'capacity'
      };
    },
    requestToMissingInstanceSpot() {
      return { high: 9n, low: 1n };
    }
  } as unknown as ZLinkBackendMeshNode;
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
          'missing'
        );
      }
    }),
    routed: {
      async sendToSpot() {
        throw new Error('Ready route must not be used.');
      },
      async requestToSpot() {
        throw new Error('Ready route must not be used.');
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => node,
    completions: () => ({
      async wait() {
        return {
          terminalResult: 0,
          failureErrno: 0,
          operationKind: 39,
          kindData: null,
          parts: encodeChannelReplyParts({
            formatMarker: 0xf2,
            kind: ZLinkChannelMessageKind.Request,
            channelName: 'mesh',
            messageName: 'Ping',
            contentType: 'application/json',
            correlationId: '1',
            deadline: null,
            topic: null,
            metadata: {}
          }, 'delayed-reply').map(part =>
            part instanceof Message ? part : toBindingMessage(part)
          )
        };
      }
    }) as never,
    defaultRequestTimeoutMs: 200
  });

  class DelayedPing {}
  await assert.rejects(
    () => address.requestToSpotAddress('delayed-room', new DelayedPing(), {
      instanceSpot: true,
      instanceSpotType: 'chat-room',
      initialMeshName: 'mesh',
      timeoutMs: 200
    }),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.CapacityExceeded
  );
  assert.equal(selectionAttempts, 1);
});

test('Missing Instance request preserves target-not-found terminal results', async () => {
  let requests = 0;
  const node = {
    instanceSpotPlacementTypes() {
      return ['chat-room'];
    },
    selectObjectPlacement() {
      return {
        kind: 'selected',
        target: {
          targetNodeRid: 'node-b',
          targetNodeGeneration: 7n,
          descriptorVersion: '11'
        }
      };
    },
    requestToMissingInstanceSpot() {
      requests += 1;
      return { high: 9n, low: 1n };
    }
  } as unknown as ZLinkBackendMeshNode;
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
          'missing'
        );
      }
    }),
    routed: {
      async sendToSpot() {
        throw new Error('Ready route must not be used.');
      },
      async requestToSpot() {
        throw new Error('Ready route must not be used.');
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => node,
    completions: () => ({
      async wait() {
        return {
          terminalResult: RequestResult.NotFound,
          failureErrno: 21,
          operationKind: 39,
          kindData: null,
          parts: []
        };
      }
    }) as never,
    defaultRequestTimeoutMs: 200
  });
  class MissingPing {}

  await assert.rejects(
    () => address.requestToSpotAddress(
      'missing-room',
      new MissingPing(),
      {
        instanceSpot: true,
        instanceSpotType: 'chat-room',
        initialMeshName: 'mesh',
        timeoutMs: 200
      }
    ),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.NotFound
  );
  assert.equal(requests, 1);
});

test('Ready Instance request ends on a disconnected stale route without resubmission', async () => {
  let invalidations = 0;
  let attempts = 0;
  const target = (nodeRid: string) => ({
    routerChannelId: 'mesh',
    targetNodeRid: nodeRid,
    spotId: 'instance-42',
    spotKind: ZLinkSpotKind.Instance,
    stableType: 'chat-room',
    targetNodeGeneration: 7n,
    targetSpotGeneration: 3n,
    authorityOwnerGeneration: 4n,
    targetOwnerId: 'owner-b',
    ownerLeaseGeneration: 5n,
    authorityStoreVersion: nodeRid === 'node-a' ? 'old' : 'new'
  });
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        return target(invalidations === 0 ? 'node-a' : 'node-b');
      },
      invalidate() {
        invalidations += 1;
      }
    }),
    routed: {
      async sendToSpot() {
        throw new Error('send is not used by this test.');
      },
      async requestToSpot<TReply>(route: unknown): Promise<TReply> {
        attempts += 1;
        if (attempts === 1) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RouteNotConnected,
            `stale route ${String((route as { targetNodeRid: string }).targetNodeRid)}`
          );
        }
        return 'unexpected' as TReply;
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 100
  });
  await assert.rejects(
    () => address.requestToSpotAddress(
      'instance-42',
      { hello: true },
      {
        instanceSpot: true,
        instanceSpotType: 'chat-room',
        initialMeshName: 'mesh'
      }
    ),
    (error: unknown) => error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.Unavailable
  );
  assert.equal(attempts, 1);
  // The positive route is reused until a specified invalidation event or
  // the failed physical admission invalidates it. The call never retries.
  assert.equal(invalidations, 1);
});

test('Instance target-not-found refreshes a Missing authority into one cold activation', async () => {
  let invalidations = 0;
  let refreshReads = 0;
  let directAttempts = 0;
  let missingAttempts = 0;
  const readyTarget = {
    routerChannelId: 'mesh',
    targetNodeRid: 'node-a',
    spotId: 'instance-42',
    spotKind: ZLinkSpotKind.Instance,
    stableType: 'chat-room',
    targetNodeGeneration: 7n,
    targetSpotGeneration: 3n,
    authorityOwnerGeneration: 4n,
    targetOwnerId: 'owner-a',
    ownerLeaseGeneration: 5n,
    authorityStoreVersion: 'old'
  };
  const resolver = {
    async resolve() {
      if (invalidations === 0) return readyTarget;
      refreshReads += 1;
      if (refreshReads === 1) return readyTarget;
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
        'authority is Missing after close'
      );
    },
    invalidate() {
      invalidations += 1;
    }
  };
  const node = {
    selectObjectPlacement(stableType: string) {
      assert.equal(stableType, 'chat-room');
      return {
        kind: 'selected',
        target: {
          targetNodeRid: 'node-b',
          targetNodeGeneration: 8n,
          descriptorVersion: '12'
        }
      };
    },
    requestToMissingInstanceSpot(
      target: { readonly targetNodeRid: string; readonly targetSpotId: string; readonly stableType: string }
    ) {
      missingAttempts += 1;
      assert.deepEqual(target, {
        targetNodeRid: 'node-b',
        targetNodeGeneration: 8n,
        targetSpotId: 'instance-42',
        stableType: 'chat-room',
        descriptorVersion: '12'
      });
      return { high: 9n, low: 1n };
    }
  } as unknown as ZLinkBackendMeshNode;
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => resolver,
    routed: {
      async sendToSpot() {
        throw new Error('send is not used by this test.');
      },
      async requestToSpot() {
        directAttempts += 1;
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
          'closed target'
        );
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => node,
    completions: () => ({
      async wait() {
        return {
          terminalResult: RequestResult.Ok,
          failureErrno: 0,
          operationKind: 39,
          kindData: null,
          parts: encodeChannelReplyParts({
            formatMarker: 0xf2,
            kind: ZLinkChannelMessageKind.Request,
            channelName: 'mesh',
            messageName: 'Lookup',
            contentType: 'application/json',
            correlationId: '1',
            deadline: null,
            topic: null,
            metadata: {}
          }, 'reactivated').map(part =>
            part instanceof Message ? part : toBindingMessage(part)
          )
        };
      }
    }) as never,
    defaultRequestTimeoutMs: 1_000
  });

  class Lookup {}
  const reply = await address.requestToSpotAddress(
    'instance-42',
    new Lookup(),
    {
      instanceSpot: true,
      instanceSpotType: 'chat-room',
      initialMeshName: 'mesh'
    }
  );

  assert.equal(reply, 'reactivated');
  assert.equal(directAttempts, 1);
  assert.equal(invalidations, 1);
  assert.equal(refreshReads, 2);
  assert.equal(missingAttempts, 1);
});

test('accepted Instance one-way admission keeps the positive route cache', async () => {
  let invalidations = 0;
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        return {
          routerChannelId: 'mesh',
          targetNodeRid: 'node-a',
          spotId: 'instance-42',
          spotKind: ZLinkSpotKind.Instance,
          stableType: 'chat-room',
          targetNodeGeneration: 7n,
          targetSpotGeneration: 3n,
          authorityOwnerGeneration: 4n,
          targetOwnerId: 'owner-a',
          ownerLeaseGeneration: 5n,
          authorityStoreVersion: 'old'
        };
      },
      invalidate() {
        invalidations += 1;
      }
    }),
    routed: {
      async sendToSpot() {
        return { status: ZLinkSubmitStatus.Submitted };
      },
      async requestToSpot() {
        throw new Error('request is not used by this test.');
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 100
  });

  assert.deepEqual(
    await address.sendToSpotAddress('instance-42', { close: true }, {
      instanceSpot: false
    }),
    { status: ZLinkSubmitStatus.Submitted }
  );
  assert.equal(invalidations, 0);
});

test('successful Instance request keeps the positive route cache', async () => {
  let invalidations = 0;
  const target = {
    routerChannelId: 'mesh',
    targetNodeRid: 'node-a',
    spotId: 'instance-42',
    spotKind: ZLinkSpotKind.Instance,
    stableType: 'chat-room',
    targetNodeGeneration: 7n,
    targetSpotGeneration: 3n,
    authorityOwnerGeneration: 4n,
    targetOwnerId: 'owner-a',
    ownerLeaseGeneration: 5n,
    authorityStoreVersion: 'old'
  };
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        return target;
      },
      invalidate() {
        invalidations += 1;
      }
    }),
    routed: {
      async sendToSpot() {
        throw new Error('send is not used by this test.');
      },
      async requestToSpot<TReply>(): Promise<TReply> {
        return 'reply' as TReply;
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 100
  });

  assert.equal(
    await address.requestToSpotAddress('instance-42', { probe: true }, {
      instanceSpot: false
    }),
    'reply'
  );
  assert.equal(invalidations, 0);
});

test('Instance one-way admission invalidates a route rejected by the target', async () => {
  let invalidations = 0;
  const address = new ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        return {
          routerChannelId: 'mesh',
          targetNodeRid: 'node-a',
          spotId: 'instance-42',
          spotKind: ZLinkSpotKind.Instance,
          stableType: 'chat-room',
          targetNodeGeneration: 7n,
          targetSpotGeneration: 3n,
          authorityOwnerGeneration: 4n,
          targetOwnerId: 'owner-a',
          ownerLeaseGeneration: 5n,
          authorityStoreVersion: 'old'
        };
      },
      invalidate() {
        invalidations += 1;
      }
    }),
    routed: {
      async sendToSpot() {
        return { status: ZLinkSubmitStatus.RouteNotConnected };
      },
      async requestToSpot() {
        throw new Error('request is not used by this test.');
      }
    },
    meshNames: () => ['mesh'],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 100
  });

  assert.deepEqual(
    await address.sendToSpotAddress('instance-42', { close: true }, {
      instanceSpot: false
    }),
    { status: ZLinkSubmitStatus.RouteNotConnected }
  );
  assert.equal(invalidations, 1);
});

test('Object Server role includes Object Client calling capability', () => {
  assert.equal(hasObjectClientCapability('none'), false);
  assert.equal(hasObjectClientCapability(undefined), false);
  assert.equal(hasObjectClientCapability('client'), true);
  assert.equal(hasObjectClientCapability('server'), true);
});

test('Ready one-way Spot send forwards application metadata through runtime route transport', async () => {
  const metadata = new Map([['trace', 'ready-send']]);
  let observed: ReadonlyMap<string, string> | undefined;
  const transport = new ZLinkRuntimeRouteTransport(() => ({
    async routeSendToSpot(
      _target: unknown,
      _packet: unknown,
      _message: unknown,
      _signal: unknown,
      forwarded: ReadonlyMap<string, string> | undefined
    ) {
      observed = forwarded;
    }
  } as never));
  const result = await transport.sendToSpot({
    routerChannelId: 'mesh',
    targetNodeRid: 'node-a',
    spotId: 'ready-room',
    spotKind: 2 as never,
    stableType: 'room',
    targetSpotGeneration: 9n
  }, { hello: true }, { metadata });
  assert.equal(result.status, ZLinkSubmitStatus.Submitted);
  assert.equal(observed, metadata);
  assert.equal(observed?.get('trace'), 'ready-send');
});

test('Ready Instance routes use command 39 with the complete authority fence', async () => {
  const metadata = new Map([['trace', 'instance-ready']]);
  const target = {
    routerChannelId: 'mesh',
    targetNodeRid: 'node-b',
    spotId: 'instance-42',
    spotKind: ZLinkSpotKind.Instance,
    stableType: 'chat-room',
    targetSpotGeneration: 17n,
    targetNodeGeneration: 19n,
    authorityOwnerGeneration: 23n,
    targetOwnerId: 'owner-b',
    ownerLeaseGeneration: 29n,
    authorityStoreVersion: 'store-31'
  } as const;
  let sentRoute: ServiceInstanceRouteFence | undefined;
  let sentMetadata: ReadonlyMap<string, string> | undefined;
  let requestedRoute: ServiceInstanceRouteFence | undefined;
  let requestedMetadata: ReadonlyMap<string, string> | undefined;
  const operation = { high: 37n, low: 41n };
  const node = {
    sendToInstanceSpot(
      route: ServiceInstanceRouteFence,
      _parts: unknown,
      _sourceSpotId: string | undefined,
      forwarded: ReadonlyMap<string, string> | undefined
    ) {
      sentRoute = route;
      sentMetadata = forwarded;
      return SubmitResult.Ok;
    },
    requestInstanceSpot(
      route: ServiceInstanceRouteFence,
      _parts: unknown,
      _timeoutMs: number | undefined,
      _sourceSpotId: string | undefined,
      forwarded: ReadonlyMap<string, string> | undefined
    ) {
      requestedRoute = route;
      requestedMetadata = forwarded;
      return operation;
    },
    entrySpot() {
      throw new Error('Ready Instance traffic must not use the generic Spot route.');
    }
  } as unknown as ZLinkBackendMeshNode;
  const completionParts = encodeChannelReplyParts({
    formatMarker: 0xf2,
    kind: ZLinkChannelMessageKind.Request,
    channelName: 'mesh',
    messageName: 'Ping',
    contentType: 'application/json',
    correlationId: '1',
    deadline: null,
    topic: null,
    metadata: {}
  }, 'instance-reply').map(part => part instanceof Message ? part : toBindingMessage(part));
  const meshSubmitters = {
    async submit(_meshName: string, attempt: () => ZLinkSubmitResult) {
      return attempt();
    }
  };
  const transport = new ZLinkRuntimeRouteTransport(
    () => undefined,
    undefined,
    () => ({
      meshNode: () => node,
      meshCompletionTable: () => ({
        async wait() {
          return {
            terminalResult: 0,
            failureErrno: 0,
            operationKind: 39,
            kindData: null,
            parts: completionParts
          };
        }
      }) as never
    }),
    undefined,
    meshSubmitters as never
  );

  const sendResult = await transport.sendToSpot(target, { hello: true }, {
    packetName: 'Ping',
    metadata
  });
  assert.equal(sendResult.status, ZLinkSubmitStatus.Submitted);
  assert.deepEqual(sentRoute, {
    targetNodeRid: 'node-b',
    targetNodeGeneration: 19n,
    targetSpotId: 'instance-42',
    objectGeneration: 17n,
    ownerId: 'owner-b',
    authorityOwnerGeneration: 23n,
    leaseGeneration: 29n,
    storeVersion: 'store-31'
  });
  assert.equal(sentMetadata, metadata);

  const reply = await transport.requestToSpot<string>(target, { hello: true }, {
    packetName: 'Ping',
    metadata
  });
  assert.equal(reply, 'instance-reply');
  assert.deepEqual(requestedRoute, sentRoute);
  assert.equal(requestedMetadata, metadata);
});

test('stateful request failure codes preserve typed Instance route errors', async () => {
  const target = {
    routerChannelId: 'mesh',
    targetNodeRid: 'node-b',
    spotId: 'instance-error',
    spotKind: ZLinkSpotKind.Instance,
    stableType: 'TenantWorker',
    targetSpotGeneration: 9n,
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 13n,
    targetOwnerId: 'owner-b',
    ownerLeaseGeneration: 17n,
    authorityStoreVersion: 'store-v9'
  } as const;
  for (const [failureCode, expectedKind, expectedPublicKind] of [
    [33, ZLinkFrameworkInternalErrorKind.SpotGenerationStale, ZLinkFrameworkErrorKind.InvalidOperation],
    [34, ZLinkFrameworkInternalErrorKind.SpotMoving, ZLinkFrameworkErrorKind.Unavailable]
  ] as const) {
    const node = {
      requestInstanceSpot() {
        return { high: 1n, low: 2n };
      },
      entrySpot() {
        throw new Error('Typed Instance route failures must use command 39.');
      }
    } as unknown as ZLinkBackendMeshNode;
    const transport = new ZLinkRuntimeRouteTransport(
      () => undefined,
      undefined,
      () => ({
        meshNode: () => node,
        meshCompletionTable: () => ({
          async wait() {
            return {
              terminalResult: RequestResult.Conflict,
              failureErrno: failureCode,
              operationKind: 39,
              kindData: null,
              parts: []
            };
          }
        }) as never
      }),
      undefined,
      {
        async submit(_meshName: string, attempt: () => ZLinkSubmitResult) {
          return attempt();
        }
      } as never
    );

    await assert.rejects(
      () => transport.requestToSpot(target, { ping: true }, { packetName: 'Ping' }),
      (error: unknown) => error instanceof ZLinkFrameworkException
        && internalFrameworkErrorKind(error) === expectedKind
        && error.kind === expectedPublicKind
    );
  }
});

function readyInstanceIngressHarness(
  current?: ServiceInstanceRouteFence,
  registerIntent = true
): {
  readonly runtime: ServiceStatefulRuntime;
  readonly ingress: (record: RawServiceIngressRecord) => string | undefined;
  readonly request: (
    route: ServiceInstanceRouteFence,
    operationKind: 'send' | 'request'
  ) => RawServiceIngressRecord;
  readonly queued: unknown[];
  readonly replies: readonly (readonly Buffer[])[];
} {
  let ingressHandler: ((record: RawServiceIngressRecord) => string | undefined) | undefined;
  const queued: unknown[] = [];
  const replies: Array<readonly Buffer[]> = [];
  const raw = {
    topology: {
      peer: (nodeRid: string) => nodeRid === 'source'
        ? { descriptor: { lifecycleGeneration: 7n } }
        : undefined
    },
    mailbox: {
      tryEnqueue: (record: unknown) => {
        queued.push(record);
        return true;
      }
    },
    setServiceIngress: (handler: typeof ingressHandler) => {
      ingressHandler = handler;
    },
    replyService: (_ingress: RawServiceIngressRecord, parts: readonly Buffer[]) => {
      replies.push(parts);
    }
  } as unknown as RawServiceMeshRuntime;
  const runtime = new ServiceStatefulRuntime(raw, 'node-a', 3n);
  if (current !== undefined) {
    runtime.restoreSpotAuthority(
      current.targetSpotId,
      'instance_spot',
      'TenantWorker',
      current.objectGeneration,
      current.authorityOwnerGeneration
    );
    if (registerIntent) runtime.registerInstanceIntent('TenantWorker', current);
  }
  return {
    runtime,
    ingress: record => {
      if (ingressHandler === undefined) throw new Error('Stateful ingress was not registered.');
      return ingressHandler(record);
    },
    request: (route, operationKind) => ({
      command: M6bServiceWireCommand.instanceSpot,
      flags: 0,
      sourceRoutingId: 'source',
      requestSequence: operationKind === 'request' ? 2n : undefined,
      parts: [
        encodeInstanceSpotHeader(
          route,
          7n,
          'source',
          undefined,
          operationKind,
          operationKind === 'request' ? { high: 1n, low: 1n } : { high: 0n, low: 0n },
          operationKind === 'request' ? 2n : undefined
        ),
        encodeApplicationPayload({
          packetName: 'ReadyInstanceApplication',
          contentType: 'application/octet-stream',
          payload: Buffer.from('payload')
        })
      ]
    }),
    queued,
    replies
  };
}

class ManualClock implements OperationClock {
  private readonly callbacks = new Map<number, () => void>();
  private nextHandle = 1;

  setTimeout(callback: () => void, _delayMs: number): number {
    const handle = this.nextHandle++;
    this.callbacks.set(handle, callback);
    return handle;
  }

  clearTimeout(handle: unknown): void {
    this.callbacks.delete(handle as number);
  }

  fireAll(): void {
    const callbacks = [...this.callbacks.values()];
    this.callbacks.clear();
    for (const callback of callbacks) callback();
  }
}

function toBindingMessage(part: ZLinkBackendMessageLike): Message {
  if (part instanceof Message) return part;
  if (typeof part === 'string' || part instanceof Uint8Array) {
    return Message.from(part);
  }
  return Message.from(Buffer.from(part.data()));
}

async function drainOne(
  backend: ZLinkNodeRawMeshBackend,
  domain: number
): Promise<ReceiveRecord> {
  let ready = backend.createReadyBatch(4);
  await pollUntil(() => {
    ready.close();
    ready = backend.createReadyBatch(4);
    return backend.drainReady(domain, ready).records.length > 0;
  });
  const claim = ready.takeClaim(0);
  const receive = backend.createReceiveBatch(4, 8, 64 * 1024);
  const result = claim.recvBatch(receive);
  assert.equal(result.ok, true);
  assert.equal(result.records.length, 1);
  claim.release();
  receive.close();
  ready.close();
  return result.records[0]!;
}

async function pollUntil(condition: () => boolean): Promise<void> {
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline) {
    if (condition()) return;
    await new Promise(resolve => setTimeout(resolve, 1));
  }
  throw new Error('Timed out waiting for M6B runtime progress.');
}

function closeParts(record: ReceiveRecord): void {
  for (const part of record.parts) part.close();
}

function createFakeStream(
  delivered: Buffer[],
  state: { disconnected: boolean }
): unknown {
  return {
    send() {
      const submit = {
        message(part: Uint8Array) {
          delivered.push(Buffer.from(part));
          return submit;
        },
        submit() {
          if (state.disconnected) throw new Error('stream route is disconnected');
          return true;
        }
      };
      return submit;
    }
  };
}

function singleAuthorityStore(
  key: string,
  snapshot: ZLinkAuthoritySnapshot
): ZLinkAuthorityStore {
  const authorityKey = { value: key } as Parameters<ZLinkAuthorityStore['readAuthority']>[0];
  return {
    async readAuthority(requested: typeof authorityKey) {
      return requested.value === key
        ? snapshot
        : { kind: 'missing', storeNow: new Date() };
    },
    async listAuthorities() {
      return {
        kind: 'page',
        items: [{
          key: authorityKey,
          snapshot
        }]
      };
    }
  } as unknown as ZLinkAuthorityStore;
}

function instanceAuthoritySnapshot(options: {
  readonly spotId: string;
  readonly meshName: string;
  readonly nodeRid: string;
  readonly storeVersion: string;
  readonly authorityOwnerGeneration: bigint;
  readonly state: 'coldActivating' | 'ready';
  readonly activationRecovery?: Parameters<
    typeof encodeServiceInstanceAuthorityPayload
  >[0]['activationRecovery'];
  readonly pendingCreation?: ZLinkAuthoritySnapshot['pendingCreation'];
}): ZLinkAuthoritySnapshot {
  const payload = encodeServiceInstanceAuthorityPayload({
    state: options.state,
    stableType: 'TenantWorker',
    spotId: options.spotId,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 5n,
    ownerMeshName: options.meshName,
    ownerNodeRid: options.nodeRid,
    ownerNodeGeneration: 1n,
    ...(options.activationRecovery === undefined
      ? {}
      : { activationRecovery: options.activationRecovery })
  });
  if (options.state === 'ready') {
    assert.equal(decodeServiceReadySpotAuthority(payload)?.spotId, options.spotId);
  } else {
    assert.equal(decodeServiceReadySpotAuthority(payload), undefined);
  }
  return {
    kind: 'snapshot',
    storeVersion: { value: options.storeVersion } as ZLinkAuthoritySnapshot['storeVersion'],
    payload,
    objectGeneration: 11n,
    authorityOwnerGeneration: options.authorityOwnerGeneration,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 5n,
    allocation: {
      // Active provider allocation alone is not the Framework Ready barrier.
      state: options.state === 'ready' ? 'active' : 'reserved',
      objectKind: 'instance_spot',
      stableType: 'TenantWorker',
      descriptor: {
        meshName: options.meshName,
        rid: options.nodeRid
      },
      descriptorLifecycleGeneration: 1n,
      capacity: {
        actors: 0,
        spots: 1,
        spotType: {
          objectKind: 'instance_spot',
          stableType: 'TenantWorker',
          count: 1
        }
      }
    },
    ...(options.pendingCreation === undefined
      ? {}
      : { pendingCreation: options.pendingCreation }),
    storeNow: new Date()
  };
}

class ReconcileAuthorityStore implements ZLinkAuthorityStore {
  readonly readKeys: string[] = [];
  readonly scanOverrides = new Map<string, ZLinkAuthoritySnapshot>();
  scanExpired = false;
  private readonly rows: Array<[string, ZLinkAuthoritySnapshot]>;

  constructor(rows: Array<[string, ZLinkAuthoritySnapshot]>) {
    this.rows = rows;
  }

  replace(key: string, snapshot: ZLinkAuthoritySnapshot): void {
    const row = this.rows.find(entry => entry[0] === key);
    if (row === undefined) throw new Error(`Missing authority row '${key}'.`);
    row[1] = snapshot;
  }

  async readAuthority(key: ZLinkAuthorityKey) {
    this.readKeys.push(key.value);
    const snapshot = this.rows.find(entry => entry[0] === key.value)?.[1];
    return snapshot ?? { kind: 'missing' as const, storeNow: new Date() };
  }

  async compareExchangeAuthority(): Promise<never> {
    throw new Error('Not used by the reconciliation test.');
  }

  async listAuthorities(
    _prefix: string,
    cursor: ZLinkAuthorityScanCursor | undefined
  ) {
    if (this.scanExpired) return { kind: 'scanExpired' as const };
    const index = cursor === undefined ? 0 : 1;
    const row = this.rows[index];
    if (row === undefined) return { kind: 'page' as const, items: [] };
    return {
      kind: 'page' as const,
      items: [{
        key: { value: row[0] } as ZLinkAuthorityKey,
        snapshot: this.scanOverrides.get(row[0]) ?? row[1]
      }],
      ...(index === 0
        ? { nextCursor: ZLinkAuthorityScanCursor.from('page-2') }
        : {})
    };
  }
}

class RecordingAuthorityNode {
  readonly recovered: Array<{
    readonly envelope: Parameters<
      ZLinkNodeRawMeshBackend['recoverInstanceActivation']
    >[0];
    readonly route: Parameters<
      ZLinkNodeRawMeshBackend['recoverInstanceActivation']
    >[1];
  }> = [];
  readonly recoveredPending: Array<{
    readonly envelope: Parameters<
      ZLinkNodeRawMeshBackend['recoverPendingInstanceActivation']
    >[0];
    readonly pending: Parameters<
      ZLinkNodeRawMeshBackend['recoverPendingInstanceActivation']
    >[1];
  }> = [];
  readonly remembered: Array<{
    readonly route: Parameters<ZLinkNodeRawMeshBackend['rememberSpotRoute']>[0];
    readonly storeVersion: string;
  }> = [];
  readonly forgotten: Array<{
    readonly spotId: string;
    readonly authorityOwnerGeneration: bigint;
    readonly storeVersion: string;
  }> = [];
  readonly intents: Array<{
    readonly instanceType: string;
    readonly route: Parameters<ZLinkNodeRawMeshBackend['registerInstanceIntent']>[1];
  }> = [];
  readonly forgottenIntents: Array<{
    readonly spotId: string;
    readonly objectGeneration: bigint;
    readonly authorityOwnerGeneration: bigint;
    readonly storeVersion: string;
  }> = [];

  constructor(
    private readonly meshName: string,
    private readonly nodeRid: string,
    private readonly completeRecovery: (
      target: Parameters<
        ZLinkNodeRawMeshBackend['completeRecoveredInstanceActivation']
      >[0],
      route: Parameters<
        ZLinkNodeRawMeshBackend['completeRecoveredInstanceActivation']
      >[1]
    ) => Promise<ServiceInstanceRouteFence> = async (_target, route) => route,
    private readonly descriptorRevision = 1n
  ) {}

  status() {
    return {
      meshName: this.meshName,
      routingId: this.nodeRid,
      lifecycleGeneration: 1n,
      descriptorRevision: this.descriptorRevision
    };
  }

  rememberSpotRoute(
    route: Parameters<ZLinkNodeRawMeshBackend['rememberSpotRoute']>[0],
    storeVersion: string
  ): void {
    this.remembered.push({ route, storeVersion });
  }

  forgetSpotRoute(
    spot: Parameters<ZLinkNodeRawMeshBackend['forgetSpotRoute']>[0],
    authorityOwnerGeneration: bigint,
    storeVersion: string
  ): void {
    this.forgotten.push({ spotId: spot.spotId, authorityOwnerGeneration, storeVersion });
  }

  registerInstanceIntent(
    instanceType: string,
    route: Parameters<ZLinkNodeRawMeshBackend['registerInstanceIntent']>[1]
  ): void {
    this.intents.push({ instanceType, route });
  }

  async recoverInstanceActivation(
    envelope: Parameters<ZLinkNodeRawMeshBackend['recoverInstanceActivation']>[0],
    route: Parameters<ZLinkNodeRawMeshBackend['recoverInstanceActivation']>[1]
  ): Promise<void> {
    this.recovered.push({ envelope, route });
  }

  async recoverPendingInstanceActivation(
    envelope: Parameters<ZLinkNodeRawMeshBackend['recoverPendingInstanceActivation']>[0],
    pending: Parameters<ZLinkNodeRawMeshBackend['recoverPendingInstanceActivation']>[1]
  ): Promise<void> {
    this.recoveredPending.push({ envelope, pending });
  }

  async completeRecoveredInstanceActivation(
    target: Parameters<
      ZLinkNodeRawMeshBackend['completeRecoveredInstanceActivation']
    >[0],
    route: Parameters<
      ZLinkNodeRawMeshBackend['completeRecoveredInstanceActivation']
    >[1]
  ): Promise<ServiceInstanceRouteFence> {
    return await this.completeRecovery(target, route);
  }

  forgetInstanceIntent(
    spotId: string,
    objectGeneration: bigint,
    authorityOwnerGeneration: bigint,
    storeVersion: string
  ): void {
    this.forgottenIntents.push({ spotId, objectGeneration, authorityOwnerGeneration, storeVersion });
  }
}

function foundBlob(bytes: Uint8Array) {
  const storeNow = new Date();
  return {
    kind: 'found' as const,
    bytes: Buffer.from(bytes),
    expiresAt: new Date(storeNow.getTime() + 60_000),
    storeNow
  };
}

function missingBlob() {
  return {
    kind: 'missing' as const,
    storeNow: new Date()
  };
}

function missingRenewal() {
  return {
    kind: 'missing' as const,
    storeNow: new Date()
  };
}
