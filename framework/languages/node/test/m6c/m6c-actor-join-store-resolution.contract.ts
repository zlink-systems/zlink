import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { RoutingId } from '../../packages/framework/src/contracts';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';
import { ZLinkConfigurationException } from '../../packages/framework/src/contracts/Configuration/ConfigurationException';
import { ZLinkRemoteActorJoinReceiver } from '../../packages/framework/src/runtime/host/remote-actor-join-receiver';
import {
  decodeActorJoin28,
  encodeActorJoin28
} from '../../../../runtime/protocol/generated/node/service_wire_pilot_codec.generated';

const actorId = 'store-resolved-actor';
const actorGeneration = 17n;
const nodeGeneration = 19n;
const authorityOwnerGeneration = 23n;
const ownerLeaseGeneration = 29n;

test('remote Actor Join resolves its stable type from a matching active Authority row', async () => {
  let resolvedType: string | undefined;
  const receiver = receiverFor({
    readAuthority: async () => authoritySnapshot(),
    getOrCreateActor: async (_actorId, stableType) => {
      resolvedType = stableType;
      return {};
    }
  });

  const result = await receiver.receive(joinPayload(), routeContext());

  assert.equal(result.accepted, false);
  assert.equal(resolvedType, 'StoreActor');
});

test('canonical actorJoin(28) prepares the Store-resolved factory without materializing an Actor', async () => {
  const prepared: string[] = [];
  let activated = false;
  const receiver = receiverFor({
    readAuthority: async () => authoritySnapshot(),
    requireRelocationActorFactory: (stableType) => {
      prepared.push(stableType);
    },
    getOrCreateActor: async () => {
      activated = true;
      return {};
    }
  });
  const fence = {
    id: actorId,
    generation: actorGeneration,
    targetNodeRid: Buffer.from('node-a'),
    targetNodeGeneration: nodeGeneration,
    expectedAuthorityOwnerGeneration: authorityOwnerGeneration,
    expectedOwnerLeaseGeneration: ownerLeaseGeneration
  } as const;
  const targetSpot = { ...fence, id: 'spot-a' };

  for (const frames of [
    encodeActorJoin28({
      correlation: 41n,
      actor: fence,
      entry: false,
      targetSpot,
      payload: {
        packetName: 'JoinRequest',
        contentType: 'application/json',
        payload: Buffer.from('{"join":true}')
      }
    }),
    encodeActorJoin28({ correlation: 42n, actor: fence, entry: true, targetSpot })
  ]) {
    const join = decodeActorJoin28(frames);
    await receiver.prepareCanonicalActorJoin({
      actorId: join.actor.id,
      actorNodeRid: Buffer.from(join.actor.targetNodeRid).toString(),
      actorGeneration: join.actor.generation,
      actorNodeGeneration: join.actor.targetNodeGeneration,
      expectedAuthorityOwnerGeneration: join.actor.expectedAuthorityOwnerGeneration,
      expectedOwnerLeaseGeneration: join.actor.expectedOwnerLeaseGeneration
    });
  }
  assert.deepEqual(prepared, ['StoreActor', 'StoreActor']);
  assert.equal(activated, false);
});

test('canonical actorJoin(28) malformed body is rejected by the generated decoder', () => {
  const malformed = Buffer.from([0x5a, 0x4d, 1, 28, 0]);
  assert.throws(() => decodeActorJoin28([malformed]));
});

test('remote Actor Join rejects an Authority fence mismatch as ProtocolError before activation', async () => {
  let activated = false;
  const receiver = receiverFor({
    readAuthority: async () => authoritySnapshot({ ownerLeaseGeneration: 31n }),
    getOrCreateActor: async () => {
      activated = true;
      return {};
    }
  });

  await assert.rejects(
    receiver.receive(joinPayload(), routeContext()),
    errorKind(ZLinkFrameworkErrorKind.ProtocolError)
  );
  assert.equal(activated, false);
});

test('remote Actor Join does not activate a forged wire stable type', async () => {
  let activated = false;
  const receiver = receiverFor({
    readAuthority: async () => authoritySnapshot(),
    getOrCreateActor: async () => {
      activated = true;
      return {};
    }
  });

  await assert.rejects(
    receiver.receive({ ...joinPayload(), actorType: 'ForgedActor' }, routeContext()),
    errorKind(ZLinkFrameworkErrorKind.TypeMismatch)
  );
  assert.equal(activated, false);
});

test('remote Actor Join reports an incomplete legacy wire fence as ProtocolError', async () => {
  const receiver = receiverFor({
    readAuthority: async () => authoritySnapshot(),
    getOrCreateActor: async () => ({})
  });
  const { expectedOwnerLeaseGeneration: _omitted, ...incomplete } = joinPayload();

  await assert.rejects(
    receiver.receive(incomplete, routeContext()),
    errorKind(ZLinkFrameworkErrorKind.ProtocolError)
  );
});

test('remote Actor Join reports a missing Authority row as NotFound', async () => {
  const receiver = receiverFor({
    readAuthority: async () => ({ kind: 'missing', storeNow: new Date() }),
    getOrCreateActor: async () => ({})
  });

  await assert.rejects(
    receiver.receive(joinPayload(), routeContext()),
    errorKind(ZLinkFrameworkErrorKind.NotFound)
  );
});

test('remote Actor Join reports an unreadable Authority row as Unavailable', async () => {
  const receiver = receiverFor({
    readAuthority: async () => {
      throw new Error('Location Store is down');
    },
    getOrCreateActor: async () => ({})
  });

  await assert.rejects(
    receiver.receive(joinPayload(), routeContext()),
    errorKind(ZLinkFrameworkErrorKind.Unavailable)
  );
});

test('remote Actor Join rejects an Authority stable type without a local factory', async () => {
  const receiver = receiverFor({
    readAuthority: async () => authoritySnapshot(),
    getOrCreateActor: async () => {
      throw new ZLinkConfigurationException("Actor factory 'StoreActor' is not registered.");
    }
  });

  await assert.rejects(
    receiver.receive(joinPayload(), routeContext()),
    errorKind(ZLinkFrameworkErrorKind.Rejected)
  );
});

function receiverFor(options: {
  readonly readAuthority: () => Promise<unknown>;
  readonly getOrCreateActor: (actorId: string, stableType: string) => Promise<unknown>;
  readonly requireRelocationActorFactory?: (stableType: string) => void;
}): ZLinkRemoteActorJoinReceiver {
  const state = {
    remoteBoundSessionTarget: undefined,
    boundSessionTransferTarget: undefined,
    setNativeActorRef: () => undefined,
    setRemoteBoundSessionTarget: () => undefined,
    setJoinedSpot: () => undefined,
    clearJoinedSpot: () => undefined
  };
  return new ZLinkRemoteActorJoinReceiver({
    authorityStore: () => ({ readAuthority: options.readAuthority } as never),
    actorManager: () => ({
      getOrCreateActor: options.getOrCreateActor,
      requireRelocationActorFactory: options.requireRelocationActorFactory ?? (() => undefined),
      getState: () => state
    } as never),
    spotManager: () => ({
      admitActorJoin: async () => ({ accepted: false })
    } as never)
  });
}

function authoritySnapshot(overrides: Record<string, unknown> = {}) {
  return {
    kind: 'snapshot',
    storeVersion: { value: 'store-version' },
    payload: Buffer.alloc(0),
    objectGeneration: actorGeneration,
    authorityOwnerGeneration,
    ownerId: 'owner-a',
    ownerLeaseGeneration,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'StoreActor',
      descriptor: { meshName: 'mesh-a', rid: 'node-a' as RoutingId },
      descriptorLifecycleGeneration: nodeGeneration,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date(),
    ...overrides
  };
}

function joinPayload() {
  return {
    packetName: '__zlink.actor.join_spot.request',
    spotId: 'spot-a',
    actorId,
    actorType: 'StoreActor',
    actorNodeRid: 'node-a',
    actorGeneration: actorGeneration.toString(),
    actorNodeGeneration: nodeGeneration.toString(),
    expectedAuthorityOwnerGeneration: authorityOwnerGeneration.toString(),
    expectedOwnerLeaseGeneration: ownerLeaseGeneration.toString(),
    request: Buffer.alloc(0).toString('base64')
  };
}

function routeContext() {
  return { sourceNodeRid: 'source-a', channelName: 'mesh-a' } as never;
}

function errorKind(kind: ZLinkFrameworkErrorKind): (error: unknown) => boolean {
  return (error): boolean => error instanceof ZLinkFrameworkException && error.kind === kind;
}
