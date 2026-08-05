const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkSpotNativeActorJoinAdmission
} = require('../../packages/framework/dist/runtime/spots/spot-native-actor-join-admission');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const {
  crc32c
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');
const {
  ZLinkEntryActorRuntimeService
} = require('../../packages/framework/dist/runtime/host/entry-actor-runtime');
const {
  ZLinkSpotActorPacketDispatch
} = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-dispatch');
const {
  runActorHandlerWithDeferredJoins
} = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
const {
  resolveLifecycleHandler
} = require('../../packages/framework/dist/runtime/handlers/handler-instance-scope');
const msgpack = require('../../packages/framework-codec-msgpack/dist/server/framework.cjs');
const protobuf = require('../../packages/framework-codec-protobuf/dist/server/framework.cjs');

function customTextSerializer(prefix = 'custom:') {
  return {
    serialize(value) {
      return framework.ZLinkEncodedPayload.from(Buffer.from(`${prefix}${value}`));
    },
    deserialize(payload) {
      const text = Buffer.from(payload.data()).toString('utf8');
      return text.startsWith(prefix) ? text.slice(prefix.length) : text;
    }
  };
}

function encodedMessage(value) {
  return framework.ZLinkMessage.fromEncoded(zlink.Message.from(value));
}

function relocationSpotNodes(stableType, implementation, adapterType) {
  return new Map([['game', {
    actorFactoryRegistrations: {
      [stableType]: {
        implementation,
        options: {},
        relocation: { kind: 'snapshot', adapterType }
      }
    }
  }]]);
}

function createActorManager(options = {}) {
  options.actorMeshNameProvider ??= () => 'play';
  return new framework.DefaultZLinkActorManager(options);
}

function lifecycleContext(actorId, actorType = 'player', membershipEpoch = 1n) {
  return {
    actorId,
    [framework.ZLINK_ACTOR_LIFECYCLE_SNAPSHOT]() {
      return {
        actorRef: {
          nodeRid: zlink.RoutingId.from('node-a'),
          actorId,
          objectGeneration: 1n,
          meshName: 'play'
        },
        actorType,
        membershipEpoch
      };
    }
  };
}

function lifecycleActor(actorId, actorType = 'player', membershipEpoch = 1n) {
  return { context: lifecycleContext(actorId, actorType, membershipEpoch) };
}

async function submitDeferredActorJoin(actor, call, decodeReply = (reply) => reply.decode()) {
  return await new Promise((resolve, reject) => {
    const previous = actor.onJoinCompleted;
    actor.onJoinCompleted = async (completion) => {
      actor.onJoinCompleted = previous;
      try {
        await previous?.call(actor, completion);
        // Deferred Join completion 계약(`status`/`actor`/`reply`/`rejection`)을
        // 그대로 넘긴다. reply는 framework message라 값 비교용으로 decode한다.
        if (completion.status === 'accepted' || completion.status === 'rejected') {
          // reply는 계약상 선택 항목이라 없을 때 키 자체를 만들지 않는다.
          const decoded = completion.reply === undefined
            ? undefined
            : { reply: decodeReply(completion.reply) };
          resolve({ ...completion, ...decoded });
          return;
        }
        reject(new framework.ZLinkFrameworkException(
          completion.kind,
          'Deferred Actor Join failed.'
        ));
      } catch (error) {
        reject(error);
      }
    };
    void runActorHandlerWithDeferredJoins(() => {
      call.defer();
    }).catch((error) => {
      actor.onJoinCompleted = previous;
      reject(error);
    });
  });
}

test('actor lifecycle snapshots are immutable identity values with distinct epoch meanings', () => {
  const actor = lifecycleActor('alice', 'player', 7n);
  const join = framework.createActorJoinRequest(actor);
  const membership = framework.createActorMembership(actor);
  const coreMembership = framework.createActorMembership(
    actor,
    {
      nodeRid: zlink.RoutingId.from('node-b'),
      actorId: 'alice',
      objectGeneration: 4n,
      meshName: 'play'
    },
    11n
  );

  assert.equal(join.actor.actorId, 'alice');
  assert.equal(join.actorType, 'player');
  assert.equal(join.expectedMembershipEpoch, 7n);
  assert.equal(membership.membershipEpoch, 7n);
  assert.equal(String(coreMembership.actor.nodeRid), 'node-b');
  assert.equal(coreMembership.actor.objectGeneration, 4n);
  assert.equal(coreMembership.membershipEpoch, 11n);
  assert.equal(Object.isFrozen(join), true);
  assert.equal(Object.isFrozen(join.actor), true);
  assert.equal(Object.isFrozen(membership), true);
  assert.equal(Object.isFrozen(membership.actor), true);
  assert.throws(
    () => framework.createActorMembership({ actorId: 'invalid' }),
    framework.ZLinkConfigurationException
  );
});

test('ZLinkActorManager create find and getOrCreate follow dotnet actor semantics', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.context = context;
    }
    configure() {
      events.push(`configure:${this.context.actorId}`);
    }
  }
  class PlayerFactory {
    async create(context, signal) {
      const { actorId } = context;
      assert.equal(signal, creation.signal);
      events.push(`create:${actorId}`);
      return new PlayerActor(actorId, context);
    }
  }

  const creation = new AbortController();
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]])
  });

  const actor = await manager.getOrCreateActor('alice', 'player', creation.signal);
  assert.equal(actor.context.actorId, 'alice');
  assert.equal(actor.context.spotId, undefined);
  assert.equal(await manager.findActor('alice'), actor);
  assert.equal(await manager.getOrCreateActor('alice', 'player'), actor);
  assert.deepEqual(events, ['create:alice', 'configure:alice']);
});

test('actor relocation terminal awaits handler cleanup and preserves state on cleanup failure', async () => {
  class PlayerActor {
    constructor(context) {
      this.context = context;
    }
  }
  class PlayerFactory {
    async create(context) {
      return new PlayerActor(context);
    }
  }

  let releaseCleanup;
  class BlockingHandler {
    dispose() {
      return new Promise((resolve) => {
        releaseCleanup = resolve;
      });
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]])
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  await resolveLifecycleHandler(actor, BlockingHandler, {
    create: (type) => new type()
  });

  const completion = manager.completeRelocationSource('alice');
  await new Promise((resolve) => setImmediate(resolve));
  assert.notEqual(manager.getState('alice'), undefined);
  releaseCleanup();
  await completion;
  assert.equal(manager.getState('alice'), undefined);

  class FailingHandler {
    dispose() {
      throw new Error('handler cleanup failed');
    }
  }
  const failedActor = await manager.getOrCreateActor('bob', 'player');
  await resolveLifecycleHandler(failedActor, FailingHandler, {
    create: (type) => new type()
  });

  await assert.rejects(
    () => manager.completeCoreRelocationSource('bob'),
    /handler cleanup failed/
  );
  assert.notEqual(manager.getState('bob'), undefined);
  await manager.completeCoreRelocationSource('bob');
  assert.equal(manager.getState('bob'), undefined);
});

test('actor transfer registry uses custom state adapters and defaults missing adapters to empty state', async () => {
  const events = [];
  class TransferActor {
    constructor(actorId, value, context) {
      this.value = value;
      this.context = context ?? { actorId };
    }
  }
  class TransferAdapter {
    async capture(actor) {
      events.push(`out:${actor.context.actorId}:${actor.value}`);
      return Buffer.from(JSON.stringify({ value: actor.value }));
    }
    async restore(actor, payload) {
      const value = JSON.parse(Buffer.from(payload).toString('utf8')).value;
      events.push(`in:${actor.context.actorId}:${value}`);
      actor.value = value;
    }
  }
  const registry = new framework.ZLinkActorTransferRegistry(
    relocationSpotNodes('transfer', TransferActor, TransferAdapter)
  );
  const source = new TransferActor('alice', 41, { actorId: 'alice' });
  const transferred = await registry.transferOut(source, 'transfer');
  assert.equal(transferred.adapterKey, 'transfer');
  const restored = new TransferActor('alice', 0, { actorId: 'alice' });
  await registry.restore(
    transferred.adapterKey,
    restored,
    transferred.state
  );
  assert.equal(restored.value, 41);

  class StatelessActor {}
  const empty = await registry.transferOut(Object.assign(new StatelessActor(), {
    actorId: 'stateless',
    context: {}
  }), 'stateless');
  assert.equal(empty.adapterKey, undefined);
  assert.equal(empty.state.toEncodedPayload().data().length, 0);
  assert.deepEqual(events, ['out:alice:41', 'in:alice:41']);
});

test('transferred actor materialization creates a fresh actor before restoring state', async () => {
  const lifecycle = [];
  class TransferActor {
    constructor(actorId, value) {
      this.actorId = actorId;
      this.value = value;
    }
  }
  class TransferAdapter {
    async capture(actor) {
      return Buffer.from(JSON.stringify({ value: actor.value }));
    }
    async restore(actor, payload) {
      actor.value = JSON.parse(Buffer.from(payload).toString('utf8')).value;
    }
  }
  class TransferFactory {
    create(context) {
      const { actorId } = context;
      lifecycle.push('factory');
      return new TransferActor(actorId, 0, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup() { return undefined; },
    createActor(actorId) {
      return { nodeRid: 'target-node', actorId, generation: 2n };
    },
    destroyActor(actorRef) {
      lifecycle.push(`destroy:${actorRef.actorId}:${actorRef.generation}`);
      return { high: 0n, low: 1n };
    }
  });
  const registry = new framework.ZLinkActorTransferRegistry(
    relocationSpotNodes('transfer', TransferActor, TransferAdapter)
  );
  const manager = createActorManager({
    actorFactories: new Map([['transfer', TransferFactory]]),
    nativeActorNode: node,
    nativeActorCompletionTableProvider: () => ({
      async wait() {
        return {
          terminalResult: 0,
          failureErrno: 0,
          operationKind: 6,
          kindData: null,
          parts: []
        };
      }
    }),
    actorTransferRegistry: registry,
    async actorCreatedNotifier() {
      lifecycle.push('onCreateActor');
    },
    actorDestroyedCleanup(actorId) {
      lifecycle.push(`cleanup:${actorId}`);
    }
  });
  const result = await manager.materializeTransferredActor(
    'alice',
    'transfer',
    'transfer',
    framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(
        Buffer.from(JSON.stringify({ value: 77 }))
      )
    )
  );
  assert.equal(result.actor.value, 77);
  assert.equal(result.actor.context, manager.getState('alice').actor.context);
  assert.equal(String(result.actorRef.nodeRid), 'target-node');
  assert.deepEqual(lifecycle, ['factory']);

  manager.getState('alice').setRemoteBoundSessionTarget({
    routerChannelId: 'session-route',
    targetNodeRid: 'session-a',
    spotId: 'session-entry'
  });
  await manager.rollbackTransferredActor(result.actor);
  assert.equal(manager.getState('alice'), undefined);
  assert.deepEqual(lifecycle, ['factory', 'destroy:alice:2', 'cleanup:alice']);
});

test('transferred actor rollback keeps a dispatch-disabled tombstone until native destroy retry succeeds', async () => {
  class TransferActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class TransferFactory {
    create(context) {
      const { actorId } = context;
      return new TransferActor(actorId, context);
    }
  }
  let destroyAttempts = 0;
  let cleanupCount = 0;
  const manager = createActorManager({
    actorFactories: new Map([['transfer', TransferFactory]]),
    nativeActorNode: createMockSpotNode({
      createActor(actorId) {
        return { nodeRid: 'target-node', actorId, generation: 2n };
      },
      destroyActor() {
        destroyAttempts += 1;
        if (destroyAttempts === 1) throw new Error('temporary native destroy failure');
        return { high: 0n, low: BigInt(destroyAttempts) };
      }
    }),
    nativeActorCompletionTableProvider: () => ({
      async wait() {
        return {
          terminalResult: 0,
          failureErrno: 0,
          operationKind: 6,
          kindData: null,
          parts: []
        };
      }
    }),
    actorDestroyedCleanup() {
      cleanupCount += 1;
    }
  });
  const { actor } = await manager.materializeTransferredActor(
    'rollback-retry',
    'transfer',
    undefined,
    framework.ZLinkMessage.fromEncoded(framework.ZLinkEncodedPayload.from(Buffer.alloc(0)))
  );

  await assert.rejects(() => manager.rollbackTransferredActor(actor), /temporary native destroy failure/);
  assert.equal(manager.getState('rollback-retry').isMoving, true);
  assert.equal(cleanupCount, 0);
  await new Promise((resolve) => setTimeout(resolve, 60));
  assert.equal(destroyAttempts, 2);
  assert.equal(manager.getState('rollback-retry'), undefined);
  assert.equal(cleanupCount, 1);
});

test('ZLinkActorManager create notifies Entry Spot after native actor creation', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.context = context;
    }
    configure() {
      events.push(`configure:${this.context.actorId}`);
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      events.push(`create:${actorId}`);
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    createActor(actorId) {
      events.push(`createNative:${actorId}`);
      return { nodeRid: 'node-a', actorId, generation: 1n };
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: node,
    async actorCreatedNotifier(nodeRid, actor) {
      events.push(`entryCreate:${nodeRid}:${actor.context.actorId}`);
    }
  });

  const actor = await manager.getOrCreateActor('alice', 'player');
  assert.equal(await manager.getOrCreateActor('alice', 'player'), actor);
  assert.deepEqual(await manager.find('alice'), {
    nodeRid: 'node-a',
    actorId: 'alice',
    objectGeneration: 1n,
    meshName: 'play'
  });
  assert.equal('actorRef' in actor.context, false);

  assert.deepEqual(events, [
    'create:alice',
    'createNative:alice',
    'entryCreate:node-a:alice',
    'configure:alice'
  ]);
});

test('ZLinkActorManager claims location before activation and releases on destroy', async () => {
  const store = new framework.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const nodeA = await locationLifecycleNode(store, 'owner-a', 'node-a');
  const nodeB = await locationLifecycleNode(store, 'owner-b', 'node-b');
  let activatedA = 0;
  let activatedB = 0;

  class PlayerFactory {
    async create(context) {
      const { actorId } = context;
      activatedA++;
      const row = await store.resolveActor({ meshName: 'play', actorId });
      assert.equal(row.ownerId, 'owner-a');
      return { actorId, context };
    }
  }
  class LosingFactory {
    create(context) {
      const { actorId } = context;
      activatedB++;
      return { actorId, context };
    }
  }

  const nativeNodeA = createMockSpotNode({
    routingId: rid('node-a'),
    createActor(actorId) {
      return { nodeRid: rid('node-a'), actorId, generation: 7n };
    },
    async destroyActor() {}
  });
  const nativeNodeB = createMockSpotNode({
    routingId: rid('node-b'),
    createActor(actorId) {
      return { nodeRid: rid('node-b'), actorId, generation: 1n };
    }
  });
  const managerA = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: nativeNodeA,
    locationLifecycle: nodeA.lifecycle
  });
  const managerB = createActorManager({
    actorFactories: new Map([['player', LosingFactory]]),
    nativeActorNode: nativeNodeB,
    locationLifecycle: nodeB.lifecycle
  });

  await managerA.create('alice', 'player').inMesh('play').submit();
  const row = await store.resolveActor({ meshName: 'play', actorId: 'alice' });
  assert.equal(row.ownerId, 'owner-a');
  assert.equal(String(row.ownerNodeRid), 'node-a');
  assert.equal(String(row.actorRef.nodeRid), 'node-a');
  assert.equal(row.actorRef.actorId, 'alice');
  assert.equal(row.actorRef.objectGeneration, 7n);
  assert.ok(managerA.getState('alice').locationGeneration > 0n);

  await assert.rejects(
    () => managerB.create('alice', 'player').inMesh('play').submit(),
    /location claim conflict/
  );
  assert.equal(activatedA, 1);
  assert.equal(activatedB, 0);

  await managerA.destroyActor(nativeNodeA, rid('node-a'), managerA.getState('alice').actor);
  assert.equal(await store.resolveActor({ meshName: 'play', actorId: 'alice' }), undefined);
});

test('remote actor takeover fences a stale source release by location generation', async () => {
  const store = new framework.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const source = await locationLifecycleNode(store, 'owner-source', 'node-source');
  const target = await locationLifecycleNode(store, 'owner-target', 'node-target');

  const sourceClaim = await source.lifecycle.claimActor('player', 'alice', rid('node-source'));
  assert.equal(sourceClaim.status, framework.ZLinkActorClaimStatus.Claimed);
  await source.lifecycle.setActorRef('player', 'alice', {
    nodeRid: rid('node-source'),
    actorId: 'alice',
    generation: 4n
  });
  const takeover = await target.lifecycle.takeoverActorJoinedSpot(
    'player',
    'alice',
    { nodeRid: rid('node-target'), actorId: 'alice', generation: 5n },
    'play',
    rid('room-target'),
    9n,
    12n,
    4n
  );
  assert.equal(takeover.status, framework.ZLinkActorClaimStatus.Claimed);
  const targetRow = await store.resolveActor({ meshName: 'play', actorId: 'alice' });
  assert.equal(targetRow.ownerId, 'owner-target');
  assert.equal(targetRow.spotKind, framework.ZLinkSpotKind.User);
  assert.equal(String(targetRow.spotId), 'room-target');
  assert.equal(takeover.generation > sourceClaim.generation, true);

  await source.lifecycle.releaseActor('player', 'alice');
  const afterStaleRelease = await store.resolveActor({ meshName: 'play', actorId: 'alice' });
  assert.equal(afterStaleRelease.ownerId, 'owner-target');
  assert.deepEqual(afterStaleRelease, targetRow);
  assert.equal(String(afterStaleRelease.actorRef.nodeRid), 'node-target');
});

test('remote actor takeover preserves moving source state until the commit reply installs the target ref', async () => {
  const store = new framework.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const source = await locationLifecycleNode(store, 'owner-source', 'node-source');
  const target = await locationLifecycleNode(store, 'owner-target', 'node-target');
  class PlayerActor {
    constructor(context) {
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      return new PlayerActor(context);
    }
  }
  let destroyedCleanup = 0;
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: createMockSpotNode({
      routingId: rid('node-source'),
      createActor(actorId) {
        return { nodeRid: rid('node-source'), actorId, generation: 1n };
      }
    }),
    locationLifecycle: source.lifecycle,
    actorDestroyedCleanup() {
      destroyedCleanup += 1;
    }
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  const state = manager.getState('alice');
  state.beginMove();

  const takeover = await target.lifecycle.takeoverActorJoinedSpot(
    'player',
    'alice',
    { nodeRid: rid('node-target'), actorId: 'alice', generation: 2n },
    'play',
    rid('room-target'),
    9n,
    12n,
    4n
  );
  await Promise.resolve();

  assert.equal(takeover.status, framework.ZLinkActorClaimStatus.Claimed);
  assert.equal(manager.getState('alice').actor, actor);
  assert.equal(destroyedCleanup, 0);
});

test('ZLinkActorManager rolls location claim back when activation fails', async () => {
  const store = new framework.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const node = await locationLifecycleNode(store, 'owner-a', 'node-a');

  class FailingFactory {
    create() {
      throw new Error('actor factory failed');
    }
  }

  const manager = createActorManager({
    actorFactories: new Map([['player', FailingFactory]]),
    nativeActorNode: createMockSpotNode({ routingId: rid('node-a') }),
    locationLifecycle: node.lifecycle
  });

  await assert.rejects(
    () => manager.create('alice', 'player').inMesh('play').submit(),
    /actor factory failed/
  );
  assert.equal(await store.resolveActor({ meshName: 'play', actorId: 'alice' }), undefined);
  assert.equal(await manager.find('alice'), undefined);
});

test('ZLinkActorManager resolves native actor node lazily at actor creation', async () => {
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  let node;
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNodeProvider: () => node
  });
  node = createMockSpotNode({
    createActor(actorId) {
      return { nodeRid: 'node-lazy', actorId, generation: 3n };
    }
  });

  const actor = await manager.getOrCreateActor('lazy', 'player');

  assert.deepEqual(await manager.find('lazy'), {
    nodeRid: 'node-lazy',
    actorId: 'lazy',
    objectGeneration: 3n,
    meshName: 'play'
  });
  assert.equal('actorRef' in actor.context, false);
});

test('ZLinkActorManager clears failed create state when Entry Spot create callback fails', async () => {
  const events = [];
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      events.push(`create:${actorId}`);
      return { actorId, context };
    }
  }
  const node = createMockSpotNode({
    createActor(actorId) {
      events.push(`createNative:${actorId}`);
      return { nodeRid: 'node-a', actorId, generation: BigInt(events.length) };
    }
  });
  let failCreateCallback = true;
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: node,
    async actorCreatedNotifier(_nodeRid, actor) {
      events.push(`entryCreate:${actor.actorId}`);
      if (failCreateCallback) {
        throw new Error('entry create failed');
      }
    }
  });

  await assert.rejects(
    () => manager.create('alice', 'player').inMesh('play').submit(),
    /creation failed/
  );
  assert.equal(await manager.find('alice'), undefined);

  failCreateCallback = false;
  const actor = await manager.getOrCreateActor('alice', 'player');

  assert.equal(actor.actorId, 'alice');
  assert.deepEqual(events, [
    'create:alice',
    'createNative:alice',
    'entryCreate:alice',
    'create:alice',
    'createNative:alice',
    'entryCreate:alice'
  ]);
});

test('ZLinkActorManager wires actor context boundSession through runtime factory', async () => {
  const sent = [];
  const boundSession = {
    send(message) {
      return {
        metadata() { return this; },
        packetName() { return this; },
        compress() { return this; },
        async submit() {
          sent.push(message);
        }
      };
    },
    async disconnect() {
      sent.push('disconnect');
    }
  };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    boundSessionFactory(actorId) {
      assert.equal(actorId, 'alice');
      return boundSession;
    }
  });

  const actor = await manager.getOrCreateActor('alice', 'player');
  assert.equal(actor.context.boundSession, boundSession);

  actor.context.boundSession.send({ ready: true }).submit();
  await actor.context.boundSession.disconnect();

  assert.deepEqual(sent, [{ ready: true }, 'disconnect']);
});

test('ZLinkActorContext exposes RouteMesh membership without Spot-owned operations', async () => {
  const events = [];
  class PlayerFactory {
    create(context) {
      return { context };
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    actorMeshNameProvider(actorType) {
      events.push(`mesh:${actorType}`);
      return 'play-mesh';
    }
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  manager.getState('alice').setJoinedSpot('room-1');

  assert.equal(actor.context.meshName, 'play-mesh');
  assert.equal(actor.context.spotId, 'room-1');
  assert.equal(actor.context.leaveSpot, undefined);
  assert.equal(actor.context.handlers, undefined);
  assert.deepEqual(events, ['mesh:player']);
});

test('Spot registry owns actor packet dispatch without Actor Context handlers', async () => {
  class ActorPingHandler {
    async handle(spot, actor, context, request) {
      return `${spot.name}:${actor.context.actorId}:${context.packetName}:${request.value}`;
    }
  }
  framework.ZLinkSpotActorRequest('ActorPingReq')(
    ActorPingHandler.prototype,
    'handle',
    Object.getOwnPropertyDescriptor(ActorPingHandler.prototype, 'handle')
  );
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]])
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime();
  registry.addHandler(ActorPingHandler);
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry,
    spot: { name: 'game' },
  });

  const reply = await dispatcher.dispatchRequest(
    actor,
    'ActorPingReq',
    { value: 'ready' }
  );

  assert.equal(actor.context.handlers, undefined);
  assert.equal(reply, 'game:alice:ActorPingReq:ready');
});

test('bound session disconnect does not destroy actor manager state or native actor', async () => {
  const events = [];
  const boundSession = {
    send() {
      throw new Error('not used');
    },
    async disconnect() {
      events.push('disconnectSession');
    }
  };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    createActor(actorId) {
      events.push(`createNative:${actorId}`);
      return { nodeRid: 'node-a', actorId, generation: 1n };
    },
    async destroyActor(actorRef) {
      events.push(`destroyNative:${actorRef.actorId}`);
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: node,
    boundSessionFactory(actorId) {
      assert.equal(actorId, 'alice');
      return boundSession;
    }
  });

  const actor = await manager.getOrCreateActor('alice', 'player');

  await actor.context.boundSession.disconnect();

  assert.equal(await manager.findActor('alice'), actor);
  assert.equal(manager.getState('alice').nativeActorRef.actorId, 'alice');
  assert.deepEqual(events, ['createNative:alice', 'disconnectSession']);
});

test('unbound actor context boundSession fails retriably until a session is bound', async () => {
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return { actorId, context };
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]])
  });
  const actor = await manager.getOrCreateActor('alice', 'player');

  assert.throws(
    () => actor.context.boundSession.send({ ready: true }),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
      && !('isRetriable' in error)
  );
  await assert.rejects(
    () => actor.context.boundSession.disconnect(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
      && !('isRetriable' in error)
  );
});

test('ZLinkActorManager rejects duplicate create and actor type mismatch', async () => {
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  class SpectatorFactory extends PlayerFactory {}
  const manager = createActorManager({
    actorFactories: new Map([
      ['player', PlayerFactory],
      ['spectator', SpectatorFactory]
    ])
  });

  await manager.create('alice', 'player').inMesh('play').submit();
  await assert.rejects(
    () => manager.create('alice', 'player').inMesh('play').submit(),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.AlreadyExists
  );
  await assert.rejects(
    () => manager.getOrCreate('alice', 'spectator').inMesh('play').submit(),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.TypeMismatch
  );
});

test('concurrent getOrCreate loser returns Existing after the winning creation commits', async () => {
  let releaseCreate;
  const release = new Promise((resolve) => {
    releaseCreate = resolve;
  });
  let createCount = 0;
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    async create(context) {
      const { actorId } = context;
      createCount += 1;
      await release;
      return new PlayerActor(actorId, context);
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]])
  });

  const first = manager.getOrCreateActorResult('alice', 'player');
  const second = manager.getOrCreateActorResult('alice', 'player');
  releaseCreate();
  const [firstResult, secondResult] = await Promise.all([first, second]);

  assert.equal(firstResult.status, 'created');
  assert.equal(secondResult.status, 'existing');
  assert.equal(firstResult.actor, secondResult.actor);
  assert.equal(createCount, 1);
});

test('distinct concurrent getOrCreate callers retry after the winning creation is rejected', async () => {
  const events = [];
  let releaseFirst;
  let firstCallbackStarted;
  const firstStarted = new Promise((resolve) => {
    firstCallbackStarted = resolve;
  });
  const firstGate = new Promise((resolve) => {
    releaseFirst = resolve;
  });
  let callbackCount = 0;
  let activeCallbacks = 0;
  let maximumActiveCallbacks = 0;
  let nativeGeneration = 0n;
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      events.push(`factory:${actorId}`);
      return { actorId, context };
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: createMockSpotNode({
      createActor(actorId) {
        nativeGeneration += 1n;
        events.push(`native-create:${nativeGeneration}`);
        return { nodeRid: rid('node-a'), actorId, generation: nativeGeneration };
      },
      async destroyActor(actorRef) {
        events.push(`native-discard:${actorRef.generation}`);
      }
    }),
    async actorCreatedNotifier(_nodeRid, actor, request) {
      callbackCount += 1;
      activeCallbacks += 1;
      maximumActiveCallbacks = Math.max(maximumActiveCallbacks, activeCallbacks);
      const requestValue = request.decode().request;
      events.push(`callback:${callbackCount}:${requestValue}`);
      try {
        if (callbackCount === 1) {
          firstCallbackStarted();
          assert.equal(await manager.findActor(actor.actorId), undefined);
          await firstGate;
          return { accepted: false, reply: { reason: 'first-rejected' } };
        }
        return { accepted: true, reply: { acceptedRequest: requestValue } };
      } finally {
        activeCallbacks -= 1;
      }
    }
  });

  const first = manager.getOrCreateActorResult('alice', 'player', { request: 'first' });
  await firstStarted;
  const second = manager.getOrCreateActorResult('alice', 'player', { request: 'second' });
  releaseFirst();

  const firstResult = await first;
  const secondResult = await second;
  assert.deepEqual(firstResult, {
    status: 'rejected',
    reply: { reason: 'first-rejected' }
  });
  assert.equal(secondResult.status, 'created');
  assert.deepEqual(secondResult.reply, { acceptedRequest: 'second' });
  assert.equal(await manager.findActor('alice'), secondResult.actor);
  assert.equal(callbackCount, 2);
  assert.equal(maximumActiveCallbacks, 1);
  assert.deepEqual(events, [
    'factory:alice',
    'native-create:1',
    'callback:1:first',
    'native-discard:1',
    'factory:alice',
    'native-create:2',
    'callback:2:second'
  ]);
});

test('rejected actor staging is never configured, found, or exposed to destroy lifecycle', async () => {
  const events = [];
  class RejectedFactory {
    create(context) {
      const { actorId } = context;
      return {
        actorId,
        context,
        configure() {
          events.push('configure');
        }
      };
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['rejected', RejectedFactory]]),
    nativeActorNode: createMockSpotNode({
      createActor(actorId) {
        return { nodeRid: rid('node-a'), actorId, generation: 9n };
      },
      async destroyActor(actorRef) {
        events.push(`discard:${actorRef.actorId}`);
      }
    }),
    async actorCreatedNotifier(_nodeRid, actor) {
      assert.equal(await manager.findActor(actor.actorId), undefined);
      return { accepted: false, reply: 'not-allowed' };
    },
    actorDestroyedCleanup() {
      events.push('destroy-lifecycle');
    }
  });

  const result = await manager.getOrCreateActorResult('denied', 'rejected');
  assert.deepEqual(result, { status: 'rejected', reply: 'not-allowed' });
  assert.equal(await manager.findActor('denied'), undefined);
  assert.equal(manager.getState('denied'), undefined);
  assert.deepEqual(events, ['discard:denied']);
});

test('ZLinkActorManager validates factory returned actor id and context', async () => {
  class WrongIdFactory {
    create(_actorId, context) {
      return { actorId: 'other', context };
    }
  }
  class WrongContextFactory {
    create(actorId) {
      return { actorId, context: {} };
    }
  }

  const manager = createActorManager({
    actorFactories: new Map([
      ['wrong-id', WrongIdFactory],
      ['wrong-context', WrongContextFactory]
    ])
  });

  for (const [actorId, actorType] of [['alice', 'wrong-id'], ['bob', 'wrong-context']]) {
    await assert.rejects(
      () => manager.create(actorId, actorType).inMesh('play').submit(),
      (error) =>
        error instanceof framework.ZLinkFrameworkException
        && error.kind === framework.ZLinkFrameworkErrorKind.InternalFailure
    );
  }
});

test('ZLinkActorDispatchMailboxSet serializes same actor and allows different actors to proceed', async () => {
  const events = [];
  const mailboxes = new framework.ZLinkActorDispatchMailboxSet();
  let releaseAlice;
  let aliceStarted;
  const aliceStartedPromise = new Promise((resolve) => {
    aliceStarted = resolve;
  });
  const releaseAlicePromise = new Promise((resolve) => {
    releaseAlice = resolve;
  });

  const aliceFirst = mailboxes.submit('alice', async () => {
    events.push('alice:first:start');
    aliceStarted();
    await releaseAlicePromise;
    events.push('alice:first:end');
  });
  await aliceStartedPromise;
  const aliceSecond = mailboxes.submit('alice', async () => {
    events.push('alice:second');
  });
  const bobFirst = mailboxes.submit('bob', async () => {
    events.push('bob:first');
  });

  await bobFirst;
  assert.deepEqual(events, ['alice:first:start', 'bob:first']);
  releaseAlice();
  await Promise.all([aliceFirst, aliceSecond]);
  assert.deepEqual(events, ['alice:first:start', 'bob:first', 'alice:first:end', 'alice:second']);
});

test('ZLinkActorContext delegates join calls to coordinator with timeout', async () => {
  const calls = [];
  // Deferred Join은 절대 deadline을 유지하므로 coordinator는 남은 시간을 받는다.
  // 네 언어 runtime이 모두 같은 의미라 정확한 ms 대신 상한만 검증한다.
  const timeouts = [];
  const replyMessage = zlink.Message.from('joined');
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const actorRef = { nodeRid: 'node-b', actorId: 'alice', generation: 1n };
  const joinCoordinator = {
    async joinSpot(actor, state, spotId, request, timeoutMs) {
      timeouts.push(timeoutMs);
      calls.push(`joinSpot:${actor.context.actorId}:${state.actorId}:${spotId}:${request.data().toString()}`);
      return { accepted: true, actor: actorRef, reply: replyMessage };
    },
    async joinEntrySpot(actor, state, nodeRid, request, timeoutMs) {
      timeouts.push(timeoutMs);
      calls.push(`joinEntry:${actor.context.actorId}:${state.actorId}:${nodeRid}:${request.data().toString()}`);
      return { accepted: true, actor: actorRef, reply: replyMessage };
    }
  };
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator
  });
  const actor = await manager.getOrCreateActor('alice', 'player');

  const request = encodedMessage('hello');
  const joinResult = await submitDeferredActorJoin(
    actor,
    actor.context.joinSpot('stage-1', request).timeout(25)
  );
  const entryRequest = encodedMessage('entry');
  const entryResult = await submitDeferredActorJoin(
    actor,
    actor.context.joinEntrySpot(entryRequest).timeout(10)
  );
  const emptyEntryResult = await submitDeferredActorJoin(
    actor,
    actor.context.joinEntrySpot().timeout(5)
  );

  assert.equal(joinResult.status, 'accepted');
  assert.deepEqual(joinResult.actor, actorRef);
  assert.deepEqual(entryResult.actor, actorRef);
  assert.deepEqual(emptyEntryResult.actor, actorRef);
  assert.deepEqual(calls, [
    'joinSpot:alice:alice:stage-1:hello',
    'joinEntry:alice:alice:undefined:entry',
    'joinEntry:alice:alice:undefined:'
  ]);
  assert.equal(timeouts.length, 3);
  for (const [index, configured] of [25, 10, 5].entries()) {
    assert.ok(timeouts[index] > 0 && timeouts[index] <= configured,
      `join timeout ${timeouts[index]} must be within (0, ${configured}]`);
  }
  // Deferred completion은 raw reply를 runtime이 닫고 framework message만 넘긴다.
  replyMessage.close();
});

test('SpotWide actor join defer yields the current Spot turn while waiting', async () => {
  const events = [];
  let releaseJoin;
  const joinGate = () => new Promise((resolve) => { releaseJoin = resolve; });
  let pendingJoin = joinGate();
  class PlayerFactory {
    create(context) {
      return { context };
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: {
      async joinSpot() {
        await pendingJoin;
        return {
          accepted: true,
          actor: { nodeRid: rid('node-a'), actorId: 'alice', generation: 1n }
        };
      }
    }
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  const serial = new framework.ZLinkSpotSerialExecutor();

  const held = serial.execute(async () => {
    events.push('defer:start');
    await runActorHandlerWithDeferredJoins(() => {
      actor.context.joinSpot('room-b').defer();
      events.push('defer:end');
    });
  });
  const afterHeld = serial.execute(() => events.push('defer:next'));
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(events, ['defer:start', 'defer:end', 'defer:next']);
  releaseJoin();
  await Promise.all([held, afterHeld]);
  assert.deepEqual(events, ['defer:start', 'defer:end', 'defer:next']);
});

test('actor manager fluent getOrCreate uses global id lookup and returns the exact ActorRef', async () => {
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return { actorId, context };
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    actorMeshNameProvider: (actorType) => actorType === 'player' ? 'play-mesh' : undefined,
    actorCreatedNodeRidProvider: () => rid('node-a')
  });

  assert.equal(await manager.find('alice'), undefined);
  const created = await manager.getOrCreate('alice', 'player')
    .inMesh('play-mesh')
    .request({ displayName: 'Alice' })
    .submit();
  assert.equal(created.status, 'created');
  const found = await manager.find('alice');
  const existing = await manager.getOrCreate('alice', 'player').inMesh('play-mesh').submit();
  assert.equal(existing.status, 'existing');
  assert.deepEqual(found, created.actor);
  assert.deepEqual(existing.actor, created.actor);
  assert.equal(String(created.actor.nodeRid), 'node-a');
  assert.equal(created.actor.actorId, 'alice');
  assert.equal(created.actor.objectGeneration, 1n);
  assert.equal(created.actor.meshName, 'play-mesh');
  assert.equal(framework.zlinkActorRefSnapshotFrom, undefined);
  assert.equal(framework.zlinkActorRefSnapshotToActorRef, undefined);
});

test('actor manager find and getOrCreate reuse a remote global location ref', async () => {
  const remote = {
    nodeRid: rid('node-b'),
    actorId: 'alice',
    objectGeneration: 7n,
    meshName: 'play-mesh'
  };
  let creates = 0;
  const manager = createActorManager({
    actorFactories: new Map([['player', {
      create() {
        creates += 1;
        return { actorId: 'alice', context: {} };
      }
    }]]),
    actorMeshNameProvider: () => 'play-mesh',
    actorCreatedNodeRidProvider: () => rid('node-a'),
    actorRefResolver: {
      async resolveActorRef(actorId) {
        return actorId === 'alice' ? remote : undefined;
      }
    }
  });

  assert.deepEqual(await manager.find('alice'), remote);
  const result = await manager.getOrCreate('alice', 'player').inMesh('play-mesh').submit();
  assert.equal(result.status, 'existing');
  assert.deepEqual(result.actor, remote);
  assert.equal(creates, 0);
});

test('actor manager resolves an Actor ID as one global identity', async () => {
  const globalRef = { nodeRid: rid('node-a'), actorId: 'shared', generation: 1n };
  const manager = createActorManager({
    actorFactories: new Map(),
    actorRefResolver: {
      async resolveActorRef(actorId) {
        return actorId === 'shared' ? globalRef : undefined;
      }
    }
  });

  assert.deepEqual(await manager.find('shared'), globalRef);
});

test('actor manager prevents the same global Actor ID from changing type or Mesh', async () => {
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return { actorId, context };
    }
  }
  const remote = { nodeRid: rid('node-b'), actorId: 'shared', generation: 9n };
  const manager = createActorManager({
    actorFactories: new Map([['player-a', PlayerFactory], ['player-b', PlayerFactory]]),
    actorMeshNameProvider: (actorType) =>
      actorType === 'player-a' ? 'mesh-a' : actorType === 'player-b' ? 'mesh-b' : undefined,
    actorCreatedNodeRidProvider: () => rid('node-a'),
    actorRefResolver: {
      async resolveActorRef(meshName, actorId) {
        return meshName === 'mesh-b' && actorId === 'shared' ? remote : undefined;
      }
    }
  });

  const local = await manager.getOrCreate('shared', 'player-a').inMesh('mesh-a').submit();

  assert.deepEqual(await manager.find('shared'), local.actor);
  await assert.rejects(
    () => manager.getOrCreate('shared', 'player-b').inMesh('mesh-b').submit(),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.TypeMismatch
  );
});

test('actor create call does not expose a caller-selected target node', () => {
  const manager = createActorManager({
    actorFactories: new Map()
  });
  const call = manager.create('alice', 'player');
  assert.equal(call.preferredNodeRid, undefined);
  assert.equal(call.onNode, undefined);
});

test('actor manager fluent create reports factory failures', async () => {
  class FailingFactory {
    create() {
      throw new Error('admission denied');
    }
  }
  const manager = createActorManager({
    actorFactories: new Map([['player', FailingFactory]]),
    actorMeshNameProvider: () => 'play-mesh'
  });

  await assert.rejects(
    () => manager.create('alice', 'player').inMesh('play-mesh').submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.InternalFailure
  );
});

test('ZLinkActorContext joinSpot uses configured custom serializer without raw request code', async () => {
  const calls = [];
  const replyMessage = zlink.Message.from('custom:joined');
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const actorRef = { nodeRid: 'node-b', actorId: 'alice', generation: 1n };
  const joinCoordinator = {
    async joinSpot(actor, state, spotId, request) {
      calls.push(`joinSpot:${actor.actorId}:${state.actorId}:${spotId}:${request.getString('utf8')}`);
      return { accepted: true, actor: actorRef, reply: replyMessage };
    },
    async joinEntrySpot() {
      throw new Error('joinEntrySpot must not be called');
    }
  };
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator,
    messageSerializers: new Map([['application/x-custom-text', customTextSerializer()]])
  });
  const actor = await manager.getOrCreateActor('alice', 'player');

  const joinResult = await submitDeferredActorJoin(
    actor,
    actor.context.joinSpot('stage-1', 'hello'),
    (reply) => customTextSerializer().deserialize(reply.toEncodedPayload())
  );

  assert.equal(joinResult.status, 'accepted');
  assert.equal(joinResult.reply, 'joined');
  assert.deepEqual(calls, ['joinSpot:alice:alice:stage-1:custom:hello']);
  replyMessage.close();
});

test('ZLinkActorContext joinSpot uses binary codec extensions without raw request code', async () => {
  const cases = [
    ['messagepack', msgpack.createMessagePackSerializer()],
    ['protobuf', protobuf.createProtobufMessageSerializer()]
  ];

  for (const [name, serializer] of cases) {
    const calls = [];
    const replyMessage = serializer.serialize({ text: `${name}:joined` });
    class PlayerActor {
      constructor(actorId, context) {
        this.actorId = actorId;
        this.context = context;
      }
    }
    class PlayerFactory {
      create(context) {
        const { actorId } = context;
        return new PlayerActor(actorId, context);
      }
    }
    const actorRef = { nodeRid: 'node-b', actorId: 'alice', generation: 1n };
    const joinCoordinator = {
      async joinSpot(actor, state, spotId, request) {
        const decoded = serializer.deserialize(request);
        calls.push(`joinSpot:${actor.actorId}:${state.actorId}:${spotId}:${decoded.text}`);
        return { accepted: true, actor: actorRef, reply: replyMessage };
      },
      async joinEntrySpot() {
        throw new Error('joinEntrySpot must not be called');
      }
    };
    const manager = createActorManager({
      actorFactories: new Map([['player', PlayerFactory]]),
      joinCoordinator,
      messageSerializers: new Map([[`application/x-test-${name}`, serializer]])
    });
    const actor = await manager.getOrCreateActor('alice', 'player');

    const joinResult = await submitDeferredActorJoin(
      actor,
      actor.context.joinSpot('stage-1', { text: `${name}:hello` }),
      (reply) => serializer.deserialize(reply.toEncodedPayload())
    );

    assert.equal(joinResult.status, 'accepted');
    assert.deepEqual(joinResult.reply, { text: `${name}:joined` });
    assert.deepEqual(calls, [`joinSpot:alice:alice:stage-1:${name}:hello`]);
  }
});

test('ZLinkActorNativeJoinCoordinator creates native actor and updates joined spot state', async () => {
  const events = [];
  const joinTimeouts = [];
  const locationWrites = [];
  const createdRef = { nodeRid: 'node-a', actorId: 'alice', generation: 1n };
  const joinedRef = { nodeRid: 'node-a', actorId: 'alice', generation: 2n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup(actorId) {
      events.push(`lookup:${actorId}`);
      return undefined;
    },
    createActor(actorId) {
      events.push(`createNative:${actorId}`);
      return createdRef;
    },
    joinActor(actorRef, targetNodeRid, targetSpotId, payload, callback, timeoutMs) {
      // Deferred Join은 절대 deadline을 유지하므로 남은 시간이 전달된다.
      joinTimeouts.push(timeoutMs);
      events.push(`join:${actorRef.generation}:${targetNodeRid}:${targetSpotId}:${payload.data().toString()}`);
      callback({
        result: 0,
        joinResultCode: 7,
        actor: joinedRef,
        joinedSpotId: targetSpotId,
        joinEpoch: 3n,
        flags: 0
      }, [zlink.Message.from('native-reply')]);
      return true;
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      completionTableProvider: () => node.completionTable,
      locationLifecycle: {
        async notifyActorJoinedSpot(...args) {
          locationWrites.push(args);
        }
      },
      spotRouteResolver: {
        async resolve(spotId) {
          return {
            routerChannelId: 'play',
            targetNodeRid: rid('node-a'),
            spotId: rid(String(spotId)),
            spotKind: framework.ZLinkSpotKind.User,
            targetSpotGeneration: 9n
          };
        }
      }
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  const request = encodedMessage('payload:hello');
  const result = await submitDeferredActorJoin(actor, actor.context.joinSpot('stage-1', request).timeout(25));

  assert.equal(result.status, 'accepted');
  assert.equal(String(result.actor.nodeRid), 'node-a');
  assert.equal(result.actor.actorId, joinedRef.actorId);
  assert.equal(result.actor.objectGeneration, joinedRef.generation);
  assert.equal(result.reply, 'native-reply');
  assert.equal(String(actor.context.spotId), 'stage-1');
  assert.equal(manager.getState('alice').nativeActorRef, joinedRef);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(locationWrites.length, 1);
  assert.equal(locationWrites[0][0], 'player');
  assert.equal(locationWrites[0][1], 'alice');
  assert.equal(locationWrites[0][2], 'play');
  assert.equal(String(locationWrites[0][3]), 'stage-1');
  assert.equal(locationWrites[0][4], 1n);
  assert.equal(locationWrites[0][5], 3n);
  assert.equal(locationWrites[0][6], 1n);
  assert.deepEqual(events, [
    'lookup:alice',
    'createNative:alice',
    'join:1:node-a:stage-1:payload:hello'
  ]);
  assert.equal(joinTimeouts.length, 1);
  assert.ok(joinTimeouts[0] > 0 && joinTimeouts[0] <= 25,
    `join timeout ${joinTimeouts[0]} must be within (0, 25]`);
});

test('ZLinkActorNativeJoinCoordinator does not resubmit a joined operation after NotConnected', async () => {
  let joinCalls = 0;
  const actorRef = { nodeRid: 'node-a', actorId: 'alice', generation: 1n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      return new PlayerActor(context.actorId, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup() {
      return undefined;
    },
    createActor() {
      return actorRef;
    },
    joinActor(_actor, targetNodeRid, targetSpotId, _request, callback) {
      joinCalls += 1;
      callback({
        result: zlink.RequestResult.NotConnected,
        joinResultCode: 0,
        actor: actorRef,
        targetNodeRid,
        joinedSpotId: targetSpotId,
        joinEpoch: 2n,
        flags: 0
      }, []);
      return true;
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      completionTableProvider: () => node.completionTable,
      spotRouteResolver: {
        async resolve(spotId) {
          return {
            routerChannelId: 'play',
            targetNodeRid: rid('node-a'),
            spotId: rid(String(spotId)),
            spotKind: framework.ZLinkSpotKind.User,
            targetSpotGeneration: 1n
          };
        }
      }
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');

  await assert.rejects(
    () => submitDeferredActorJoin(actor, actor.context.joinSpot('stage-1', encodedMessage('payload')).timeout(25)),
    /Deferred Actor Join failed/
  );
  assert.equal(joinCalls, 1);
});

test('ZLinkActorNativeJoinCoordinator uses the formal Core operation for a remote join', async () => {
  const events = [];
  const coordinatorTimeouts = [];
  const createdRef = { nodeRid: rid('node-b'), actorId: 'alice', generation: 1n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup() {
      return undefined;
    },
    createActor() {
      return createdRef;
    },
    rememberSpotRoute(route) {
      events.push(
        `rememberSpot:${route.spot.spotId}:${route.spot.generation}:`
        + `${route.targetNodeRid}:${route.targetNodeGeneration}:`
        + `${route.authorityOwnerGeneration}:${route.storeVersion}`
      );
    },
    joinActor(actorRef, targetNodeRid, targetSpotId, request, callback, timeoutMs) {
      // Join call은 기본 5초 deadline을 유지하므로 남은 시간이 전달된다.
      coordinatorTimeouts.push(timeoutMs);
      events.push(`joinActor:${actorRef.generation}:${targetNodeRid}:${targetSpotId}:${request.data().toString()}`);
      callback({
        result: 0,
        joinResultCode: 0,
        actor: { nodeRid: rid('node-a'), actorId: 'alice', generation: 2n },
        targetNodeRid,
        joinedSpotId: targetSpotId,
        joinEpoch: 3n,
        flags: 0
      }, [zlink.Message.from('remote-reply')]);
      return true;
    }
  });
  const spotRouteResolver = {
    async resolve(spotId) {
      events.push(`resolve:${spotId}`);
      return {
        routerChannelId: 'play-node',
        targetNodeRid: 'node-a',
        spotId,
        spotKind: framework.ZLinkSpotKind.User,
        targetSpotGeneration: 9n,
        targetNodeGeneration: 4n,
        authorityOwnerGeneration: 5n,
        // A route fence needs the full generation set. Omitting the owner lease
        // generation makes the fence unsound, so the runtime skips remembering
        // the route entirely.
        ownerLeaseGeneration: 7n,
        authorityStoreVersion: 'store-6'
      };
    }
  };
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      completionTableProvider: () => node.completionTable,
      spotRouteResolver,
      routedTransport: {
        canRoutePacketChannel(routerChannelId) {
          events.push(`canRoutePacket:${routerChannelId}`);
          return false;
        },
        canRouteChannel(routerChannelId) {
          events.push(`canRoute:${routerChannelId}`);
          return false;
        },
        async request() {
          throw new Error('route channel request must not be used for spot-node mesh joins');
        }
      },
      async remoteActorBinder(actorRef) {
        assert.equal(actorRef.nodeRid instanceof zlink.RoutingId, true);
        events.push(`bind:${actorRef.nodeRid}:${actorRef.actorId}:${actorRef.objectGeneration}`);
      }
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  const request = encodedMessage('payload');
  const result = await submitDeferredActorJoin(actor, actor.context.joinSpot('room-1', request));
  assert.equal(result.status, 'accepted');
  assert.equal(result.reply, 'remote-reply');
  assert.deepEqual(events, [
    'resolve:room-1',
    'rememberSpot:room-1:9:node-a:4:5:store-6',
    'joinActor:1:node-a:room-1:payload',
    'bind:node-a:alice:2'
  ]);
});

test('ZLinkActorNativeJoinCoordinator keeps remote joins on the formal Core surface when a routed transport exists', async () => {
  const events = [];
  const createdRef = { nodeRid: 'node-b', actorId: 'alice', generation: 1n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup() {
      return undefined;
    },
    createActor() {
      return createdRef;
    },
    entrySpot() {
      return { routingId: 'node-b-entry' };
    },
    joinActor(actorRef, targetNodeRid, targetSpotId, request, callback) {
      events.push(`formalJoin:${targetNodeRid}:${targetSpotId}:${request.data().toString()}`);
      callback({
        result: 0,
        joinResultCode: 0,
        actor: { ...actorRef, nodeRid: targetNodeRid, generation: 2n },
        joinedSpotId: targetSpotId,
        joinEpoch: 3n
      }, [zlink.Message.from('routed-reply')]);
      return true;
    }
  });
  const spotRouteResolver = {
    async resolve(spotId) {
      events.push(`resolve:${spotId}`);
      return {
        routerChannelId: 'play-node',
        targetNodeRid: 'node-a',
        spotId,
        spotKind: framework.ZLinkSpotKind.User,
        targetSpotGeneration: 9n
      };
    }
  };
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      completionTableProvider: () => node.completionTable,
      spotRouteResolver,
      routedTransport: {
        canRoutePacketChannel(routerChannelId) {
          events.push(`canRoutePacket:${routerChannelId}`);
          return true;
        },
        canRouteChannel(routerChannelId) {
          events.push(`canRoute:${routerChannelId}`);
          return true;
        },
        async request(routerChannelId, targetNodeRid, packetName, payload) {
          events.push(`routeRequest:${payload.phase}:${routerChannelId}:${targetNodeRid}:${payload.spotId}:${packetName}:${payload.actorId}:${payload.actorType}:${Buffer.from(payload.request, 'base64').toString()}`);
          if (payload.phase === 'admission') {
            assert.equal(typeof payload.transferId, 'string');
            return {
              accepted: true,
              actorNodeRid: 'node-b',
              actorId: 'alice',
              actorGeneration: '1',
              reply: Buffer.from('routed-reply').toString('base64')
            };
          }
          assert.equal(payload.phase, 'commit');
          assert.equal(payload.transferState, '');
          return {
            accepted: true,
            actorNodeRid: 'node-a',
            actorId: 'alice',
            actorGeneration: '2',
            reply: Buffer.from('routed-reply').toString('base64')
          };
        }
      },
      async remoteActorBinder(actorRef) {
        events.push(`bind:${actorRef.nodeRid}:${actorRef.actorId}:${actorRef.objectGeneration}`);
      },
      sourceTransfer: {
        async prepareSource(joinedActor) {
          return {
            state: framework.ZLinkMessage.fromEncoded(
              framework.ZLinkEncodedPayload.from(Buffer.alloc(0))
            ),
            handoffBacklog: [],
            commit() {
              events.push(`leave:${joinedActor.actorId}`);
            },
            async rollback() {}
          };
        }
      }
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  const request = encodedMessage('payload');
  const result = await submitDeferredActorJoin(actor, actor.context.joinSpot('room-1', request));

  assert.equal(result.status, 'accepted');
  assert.equal(result.reply, 'routed-reply');
  assert.deepEqual(events, [
    'resolve:room-1',
    'formalJoin:node-a:room-1:payload',
    'bind:node-a:alice:2'
  ]);
});

test('remote transfer failures before commit preserve source ownership and never bind the target', async () => {
  async function runFailure(failurePoint) {
    const events = [];
    class PlayerActor {
      constructor(actorId, context) {
        this.actorId = actorId;
        this.context = context;
      }
    }
    class PlayerFactory {
      create(context) {
        const { actorId } = context;
        return new PlayerActor(actorId, context);
      }
    }
    class PlayerTransferAdapter {
      async capture() {
        events.push('transferOut');
        if (failurePoint === 'transferOut') {
          throw new Error('injected transferOut failure');
        }
        return Buffer.from(JSON.stringify({ version: 1 }));
      }
      async restore() {
        throw new Error('target transferIn is not part of the source contract test');
      }
    }
    const node = createMockSpotNode({
      routingId: rid('node-source'),
      entrySpot() {
        return { routingId: rid('entry-source') };
      },
      createActor(actorId) {
        return { nodeRid: rid('node-source'), actorId, generation: 1n };
      },
      joinActor(actorRef, targetNodeRid, targetSpotId, request, callback) {
        const payload = JSON.parse(request.data().toString());
        if (payload.phase === 'admission') {
          events.push('formalJoin:admission');
          callback({
            result: 0,
            joinResultCode: 0,
            actor: { ...actorRef, nodeRid: targetNodeRid, generation: 2n },
            joinedSpotId: targetSpotId,
            joinEpoch: 1n
          }, []);
          return true;
        }
        events.push('formalJoin:failed');
        throw new Error(`injected ${failurePoint} failure`);
      }
    });
    const transferRegistry = new framework.ZLinkActorTransferRegistry(
      relocationSpotNodes('player', PlayerActor, PlayerTransferAdapter)
    );
    const manager = createActorManager({
      actorFactories: new Map([['player', PlayerFactory]]),
      joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
        node,
        completionTableProvider: () => node.completionTable,
        spotRouteResolver: {
          async resolve(spotId) {
            return {
              routerChannelId: 'play',
              targetNodeRid: rid('node-target'),
              spotId,
              spotKind: framework.ZLinkSpotKind.User,
              targetSpotGeneration: 9n
            };
          }
        },
        routedTransport: {
          canRoutePacketChannel() { return true; },
          async request(_channel, _nodeRid, _packetName, payload) {
            events.push(`request:${payload.phase}`);
            assert.equal(payload.phase, 'admission');
            return {
              accepted: true,
              actorNodeRid: 'node-source',
              actorId: 'alice',
              actorGeneration: '1'
            };
          }
        },
        sourceTransfer: {
          async prepareSource(joinedActor, state, signal) {
            events.push('move:start');
            state.beginMove();
            try {
              const transfer = await transferRegistry.transferOut(
                joinedActor,
                'player',
                signal
              );
              events.push('prepare');
              if (failurePoint === 'prepare') {
                throw new Error('injected prepare failure');
              }
              return {
                ...transfer,
                handoffBacklog: [],
                commit() {},
                async rollback() {
                  events.push('move:cancel');
                  state.endMove();
                }
              };
            } catch (error) {
              events.push('move:cancel');
              state.endMove();
              throw error;
            }
          }
        },
        async remoteActorBinder() {
          events.push('bind');
        }
      })
    });
    const actor = await manager.getOrCreateActor('alice', 'player');
    // Deferred Join의 failed completion 계약은 public kind만 전달한다.
    // 실패 지점 구분은 아래 events 단정이 소유한다.
    await assert.rejects(
      () => submitDeferredActorJoin(actor, actor.context.joinSpot('room-target', encodedMessage('join'))),
      (error) => error instanceof framework.ZLinkFrameworkException
    );
    assert.equal(manager.getState('alice').isMoving, false);
    assert.equal(manager.getState('alice').nativeActorRef.nodeRid.toHex(), rid('node-source').toHex());
    assert.equal(events.includes('bind'), false);
    assert.equal(events.some((event) => event === 'request:commit'), false);
    return events;
  }

  assert.deepEqual(await runFailure('transferOut'), [
    'formalJoin:admission',
    'move:start',
    'transferOut',
    'move:cancel'
  ]);
  assert.deepEqual(await runFailure('prepare'), [
    'formalJoin:admission',
    'move:start',
    'transferOut',
    'prepare',
    'move:cancel'
  ]);
});

test('formal remote join rejection rolls back prepared source movement and preserves ownership', async () => {
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) { return new PlayerActor(context.actorId, context); }
  }
  const node = createMockSpotNode({
    routingId: rid('node-source'),
    createActor(actorId) { return { nodeRid: rid('node-source'), actorId, generation: 1n }; },
    joinActor(actorRef, targetNodeRid, targetSpotId, request, callback) {
      const payload = JSON.parse(request.data().toString());
      callback({
        result: 0,
        joinResultCode: payload.phase === 'admission' ? 0 : 1,
        actor: { ...actorRef, nodeRid: targetNodeRid, generation: 2n },
        joinedSpotId: targetSpotId,
        joinEpoch: 1n
      }, []);
      return true;
    }
  });
  let rolledBack = false;
  let committed = false;
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      entrySpotIdProvider: () => 'play-entry-123e4567-e89b-42d3-a456-426614174000',
      completionTableProvider: () => node.completionTable,
      spotRouteResolver: {
        async resolve(spotId) {
          return {
            routerChannelId: 'play',
            targetNodeRid: rid('node-target'),
            spotId,
            spotKind: framework.ZLinkSpotKind.User,
            targetSpotGeneration: 9n
          };
        }
      },
      routedTransport: {
        canRoutePacketChannel() { return true; },
        async request(_channel, _nodeRid, _packetName, payload) {
          return payload.phase === 'admission'
            ? {
                accepted: true,
                actorNodeRid: 'node-source',
                actorId: 'alice',
                actorGeneration: '1'
              }
            : {
                accepted: false,
                actorNodeRid: 'node-source',
                actorId: 'alice',
                actorGeneration: '1'
              };
        }
      },
      sourceTransfer: {
        async prepareSource(_actor, state) {
          state.beginMove();
          return {
            state: framework.ZLinkMessage.from({ version: 1 }),
            handoffBacklog: [],
            async reserveTarget() {},
            commit() { committed = true; },
            async rollback() {
              rolledBack = true;
              state.endMove();
            }
          };
        }
      }
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');

  const result = await submitDeferredActorJoin(
    actor,
    actor.context.joinSpot('room-target', encodedMessage('join'))
  );
  assert.equal(result.status, 'rejected');
  assert.equal(Object.hasOwn(result, 'reply'), false);
  assert.equal(rolledBack, true);
  assert.equal(committed, false);
  assert.equal(manager.getState('alice').isMoving, false);
  assert.equal(manager.getState('alice').nativeActorRef.nodeRid.toHex(), rid('node-source').toHex());
});

test('target ownership publication retries an exact command 44 after command 45 ACK loss', async () => {
  const actor = { context: { actorId: 'actor-ack' } };
  const acceptedJournal = Buffer.from(JSON.stringify({
    version: 1,
    actorId: 'actor-ack',
    actorGeneration: '9',
    sealId: 'seal-ack',
    acceptedHighWater: '41',
    entries: []
  }));
  const acceptedJournalChecksum = crc32c(acceptedJournal);
  const state = {
    nativeActorRef: { nodeRid: rid('target-node'), actorId: 'actor-ack', generation: 9n },
    locationGeneration: 17n,
    ownerLeaseGeneration: 23n,
    spotId: 'room-target',
    remoteBoundSessionTarget: {
      routerChannelId: 'session.route',
      targetNodeRid: rid('session-node'),
      spotId: 'session-entry',
      bindingGeneration: 5n,
      previousAuthorityOwnerGeneration: 16n,
      previousOwnerLeaseGeneration: 22n,
      acceptedHighWater: 41n,
      relocationSealId: 'seal-ack',
      acceptedJournalReference: 'journal-ack',
      acceptedJournalChecksumCrc32c: acceptedJournalChecksum
    }
  };
  const requests = [];
  const reported = [];
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {
      async sendToSpot() { throw new Error('command 44 must use request/reply transport'); },
      async requestToSpot(target, payload, options) {
        requests.push({ target, payload, options });
        if (requests.length === 1) throw new Error('command 45 ACK lost');
        return {
          actorId: payload.actorId,
          actorGeneration: payload.actorGeneration,
          actorOwnershipGeneration: payload.actorOwnershipGeneration,
          bindingGeneration: payload.bindingGeneration,
          targetOwnerLeaseGeneration: payload.targetOwnerLeaseGeneration,
          acceptedHighWater: payload.acceptedHighWater,
          sealId: payload.sealId
        };
      }
    },
    spotManager: () => undefined,
    actorManager: () => ({ getState: () => state }),
    primaryMeshNode: () => ({}),
    async notifyEntrySpotActorLeft() {},
    async restoreEntrySpotActorJoined() {},
    locationLifecycle: () => undefined,
    actorHandoff: {},
    actorTransferRegistry: {},
    authorityStore: () => undefined,
    relocationStore: () => ({
      async read(reference) {
        assert.equal(reference.value, 'journal-ack');
        return foundBlob(acceptedJournal);
      }
    }),
    reportPostCommitError(error) { reported.push(error); },
    clearRemoteActorPacketTarget() {}
  });

  await runtime.publishRoutedActorOwnership(actor);

  assert.equal(requests.length, 2);
  assert.equal(reported.length, 1);
  assert.deepEqual(requests[1], requests[0]);
  assert.equal(requests[1].options.packetName, framework.ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET);
  assert.equal(requests[1].payload.actorId, 'actor-ack');
  assert.equal(requests[1].payload.actorGeneration, '9');
  assert.equal(requests[1].payload.previousActorOwnershipGeneration, '16');
  assert.equal(requests[1].payload.actorOwnershipGeneration, '17');
  assert.equal(requests[1].payload.bindingGeneration, '5');
  assert.equal(requests[1].payload.previousOwnerLeaseGeneration, '22');
  assert.equal(requests[1].payload.targetOwnerLeaseGeneration, '23');
  assert.equal(requests[1].payload.acceptedHighWater, '41');
  assert.equal(requests[1].payload.sealId, 'seal-ack');
  assert.equal(requests[1].payload.acceptedJournalReference, 'journal-ack');
  assert.equal(requests[1].payload.acceptedJournalChecksumCrc32c, acceptedJournalChecksum);
});

test('ordinary remote Session binding does not enter the relocation journal path on a Spot join', async () => {
  const actor = { context: { actorId: 'actor-initial-bind' } };
  let ownershipRequests = 0;
  const state = {
    nativeActorRef: { nodeRid: rid('play-node'), actorId: actor.context.actorId, generation: 9n },
    locationGeneration: 17n,
    ownerLeaseGeneration: 23n,
    spotId: rid('room-target'),
    remoteBoundSessionTarget: {
      routerChannelId: 'session.route',
      targetNodeRid: rid('session-node'),
      spotId: rid('session-entry'),
      sessionNodeRid: rid('session-node'),
      sessionRid: rid('session-rid'),
      bindingGeneration: 5n
    }
  };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {
      async requestToSpot() {
        ownershipRequests += 1;
        throw new Error('ordinary Session binding must not publish command 44');
      }
    },
    spotManager: () => undefined,
    actorManager: () => ({ getState: () => state }),
    primaryMeshNode: () => ({}),
    async notifyEntrySpotActorLeft() {},
    async restoreEntrySpotActorJoined() {},
    locationLifecycle: () => undefined,
    actorHandoff: {},
    actorTransferRegistry: {},
    authorityStore: () => undefined,
    relocationStore: () => undefined,
    clearRemoteActorPacketTarget() {}
  });

  await runtime.publishRoutedActorOwnership(actor);
  assert.equal(ownershipRequests, 0);
});

test('source command 42 seal publishes a durable accepted journal and rollback aborts it', async () => {
  const actor = { context: { actorId: 'actor-seal' } };
  let moving = false;
  let remoteTarget = {
    routerChannelId: 'session.route',
    targetNodeRid: rid('session-node'),
    spotId: 'session-entry',
    bindingGeneration: 11n
  };
  const state = {
    actorType: 'player',
    nativeActorRef: { nodeRid: rid('source-node'), actorId: 'actor-seal', generation: 9n },
    locationGeneration: 3n,
    ownerLeaseGeneration: 5n,
    get remoteBoundSessionTarget() { return remoteTarget; },
    setRemoteBoundSessionTarget(value) { remoteTarget = value; },
    beginMove() { assert.equal(moving, false); moving = true; },
    endMove() { moving = false; }
  };
  const commands = [];
  const stored = new Map();
  let deleted = 0;
  const relocationStore = {
    async put(reference, payload) {
      stored.set(reference.value, Buffer.from(payload));
      const storeNow = new Date();
      return {
        kind: 'stored',
        expiresAt: new Date(storeNow.getTime() + 60_000),
        storeNow
      };
    },
    async read(reference) {
      const payload = stored.get(reference.value);
      return payload === undefined ? missingBlob() : foundBlob(payload);
    },
    async delete(reference) {
      deleted++;
      stored.delete(reference.value);
    }
  };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {
      async requestToSpot(_target, payload, options) {
        commands.push({ payload, options });
        return {
          actorId: payload.actorId,
          sealId: payload.sealId,
          acceptedHighWater: options.packetName === framework.ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET ? '7' : '0'
        };
      }
    },
    spotManager: () => undefined,
    actorManager: () => ({ getState: () => state }),
    primaryMeshNode: () => ({}),
    async notifyEntrySpotActorLeft() {},
    async restoreEntrySpotActorJoined() {},
    locationLifecycle: () => undefined,
    actorHandoff: {
      begin() {},
      isActive() { return false; },
      snapshotCoreBacklog() { return [{ index: 0, header: 'aA==', payload: 'Yg==', returnResponse: false }]; },
      cancel() {},
      pendingCount() { return 1; }
    },
    actorTransferRegistry: {
      async transferOut() { return { state: encodedMessage('state') }; }
    },
    authorityStore: () => undefined,
    relocationStore: () => relocationStore,
    clearRemoteActorPacketTarget() {}
  });

  const prepared = await runtime.prepareSource(actor, state, undefined, 'core');
  assert.equal(commands[0].options.packetName, framework.ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET);
  assert.equal(remoteTarget.acceptedHighWater, 7n);
  assert.equal(stored.has(remoteTarget.acceptedJournalReference), true);
  assert.equal(remoteTarget.relocationSealId, commands[0].payload.sealId);
  assert.equal(stored.size, 1);
  assert.equal(moving, true);

  await prepared.rollback();
  assert.equal(commands[1].options.packetName, framework.ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET);
  assert.equal(deleted, 1);
  assert.equal(stored.size, 0);
  assert.equal(moving, false);
});

test('target route opening retries the exact released seal after its ACK is lost', async () => {
  const actor = { context: { actorId: 'actor-open-retry' } };
  const state = {
    nativeActorRef: { nodeRid: rid('target-node'), actorId: 'actor-open-retry', generation: 9n },
    remoteBoundSessionTarget: {
      routerChannelId: 'session.route',
      targetNodeRid: rid('session-node'),
      spotId: 'session-entry',
      bindingGeneration: 5n,
      previousAuthorityOwnerGeneration: 16n,
      previousOwnerLeaseGeneration: 22n,
      relocationSealId: 'seal-open-retry'
    }
  };
  const requests = [];
  const reported = [];
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {
      async requestToSpot(target, payload, options) {
        requests.push({ target, payload, options });
        if (requests.length === 1) throw new Error('open ACK lost');
        return { actorId: payload.actorId, sealId: payload.sealId, acceptedHighWater: '0' };
      }
    },
    spotManager: () => undefined,
    actorManager: () => ({ getState: () => state }),
    primaryMeshNode: () => ({}),
    async notifyEntrySpotActorLeft() {},
    async restoreEntrySpotActorJoined() {},
    locationLifecycle: () => undefined,
    actorHandoff: {},
    actorTransferRegistry: {},
    authorityStore: () => undefined,
    relocationStore: () => undefined,
    reportPostCommitError(error) { reported.push(error); },
    clearRemoteActorPacketTarget() {}
  });

  await runtime.openRoutedActorSession(actor);

  assert.equal(requests.length, 2);
  assert.equal(reported.length, 1);
  assert.deepEqual(requests[1], requests[0]);
  assert.equal(requests[1].options.packetName, framework.ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET);
  assert.equal(requests[1].payload.sealId, 'seal-open-retry');
});

test('transferred Actor admission remains sealed until command 45 ACK completes', async () => {
  const actor = { context: lifecycleContext('actor-steady') };
  let joined;
  let sealed = false;
  let releaseAck;
  let publishStarted;
  let releaseOpen;
  let openStarted;
  const started = new Promise((resolve) => { publishStarted = resolve; });
  const ackBlocked = new Promise((resolve) => { releaseAck = resolve; });
  const opening = new Promise((resolve) => { openStarted = resolve; });
  const openBlocked = new Promise((resolve) => { releaseOpen = resolve; });
  const activation = {
    spotId: 'room-target',
    meshName: 'play',
    spot: { async onJoinedActor() {} },
    serial: { async execute(operation) { return await operation(); } },
    commitActorJoin(value) { joined = value; },
    beginActorTransfer() { sealed = true; },
    cancelActorTransfer() { sealed = false; },
    commitActorDeparture() { joined = undefined; sealed = true; },
    resolveJoinedActor(actorId) {
      return !sealed && joined?.context.actorId === actorId ? joined : undefined;
    }
  };
  const coordinator = new framework.ZLinkSpotActorAdmissionCoordinator({
    actorTransferRuntime: {
      commitRoutedActor() {},
      async claimRoutedActorLocation() {},
      async publishRoutedActorOwnership() {
        publishStarted();
        await ackBlocked;
      },
      async openRoutedActorSession() {
        openStarted();
        await openBlocked;
      },
      clearRoutedActor() {},
      async rollbackRoutedActor() {}
    }
  });

  const committing = coordinator.commitTransferredActorTransaction(activation, actor, []);
  await started;
  assert.equal(activation.resolveJoinedActor(actor.context.actorId), undefined);

  releaseAck();
  await opening;
  assert.equal(activation.resolveJoinedActor(actor.context.actorId), undefined);
  releaseOpen();
  assert.deepEqual(await committing, []);
  assert.equal(activation.resolveJoinedActor(actor.context.actorId), actor);
});

test('command 44 completion never rolls the committed target back when route opening fails', async () => {
  const actor = { context: lifecycleContext('actor-post-commit') };
  let joined;
  let sealed = false;
  let departures = 0;
  let clears = 0;
  let rollbacks = 0;
  const activation = {
    spotId: 'room-target',
    meshName: 'play',
    spot: { async onJoinedActor() {} },
    serial: { async execute(operation) { return await operation(); } },
    commitActorJoin(value) { joined = value; },
    beginActorTransfer() { sealed = true; },
    cancelActorTransfer() { sealed = false; },
    commitActorDeparture() { departures++; joined = undefined; },
    resolveJoinedActor(actorId) {
      return !sealed && joined?.context.actorId === actorId ? joined : undefined;
    }
  };
  const coordinator = new framework.ZLinkSpotActorAdmissionCoordinator({
    actorTransferRuntime: {
      commitRoutedActor() {},
      async claimRoutedActorLocation() {},
      async publishRoutedActorOwnership() {},
      async openRoutedActorSession() { throw new Error('route open interrupted'); },
      clearRoutedActor() { clears++; },
      async rollbackRoutedActor() { rollbacks++; }
    }
  });

  await assert.rejects(
    coordinator.commitTransferredActorTransaction(activation, actor, []),
    /route open interrupted/
  );
  assert.equal(departures, 0);
  assert.equal(clears, 0);
  assert.equal(rollbacks, 0);
  assert.equal(joined, actor);
  assert.equal(sealed, true);
});

test('target ownership publication shutdown preserves the committed target location', async () => {
  const actor = { context: { actorId: 'alice' } };
  const acceptedJournal = Buffer.from(JSON.stringify({
    version: 1,
    actorId: 'alice',
    actorGeneration: '9',
    sealId: 'seal-alice',
    acceptedHighWater: '41',
    entries: []
  }));
  const acceptedJournalChecksum = crc32c(acceptedJournal);
  let released = 0;
  let ownsLocation = false;
  let locationGeneration;
  const shutdown = new AbortController();
  const state = {
    actorType: 'player',
    nativeActorRef: { nodeRid: rid('target-node'), actorId: 'alice', generation: 9n },
    remoteBoundSessionTarget: {
      routerChannelId: 'session.route',
      targetNodeRid: rid('source-node'),
      spotId: rid('source-entry'),
      bindingGeneration: 5n,
      previousAuthorityOwnerGeneration: 16n,
      previousOwnerLeaseGeneration: 22n,
      acceptedHighWater: 41n,
      relocationSealId: 'seal-alice',
      acceptedJournalReference: 'journal-alice',
      acceptedJournalChecksumCrc32c: acceptedJournalChecksum
    },
    clearAfterDestroy() {},
    setJoinedSpot() {},
    get locationGeneration() { return locationGeneration; },
    setLocationGeneration(value) { locationGeneration = value; },
    ownerLeaseGeneration: 23n,
    setOwnerLeaseGeneration(value) { this.ownerLeaseGeneration = value; },
    get ownsLocation() { return ownsLocation; },
    markLocationOwned() { ownsLocation = true; },
    markLocationReleased() { ownsLocation = false; }
  };
  const lifecycle = {
    async takeoverActorJoinedSpot(
      actorType,
      actorId,
      actorRef,
      meshName,
      spotId,
      spotGeneration,
      membershipEpoch,
      ownerNodeGeneration
    ) {
      assert.equal(actorType, 'player');
      assert.equal(actorId, 'alice');
      assert.equal(String(actorRef.nodeRid), 'target-node');
      assert.equal(meshName, 'play');
      assert.equal(String(spotId), 'room');
      assert.equal(spotGeneration, 9n);
      assert.equal(membershipEpoch, 12n);
      assert.equal(ownerNodeGeneration, 4n);
      return { status: 'claimed', generation: 17n, claimed: { leaseGeneration: 23n } };
    },
    async releaseActor() { released++; },
    async releaseActorEventually() { throw new Error('not used'); }
  };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {
      async requestToSpot() {
        shutdown.abort();
        throw new Error('ownership route unavailable');
      }
    },
    spotManager: () => undefined,
    actorManager: () => ({
      getState: () => state,
      async rollbackTransferredActor() {}
    }),
    primaryMeshNode: () => ({
      actorLookup() {
        return {
          actor: state.nativeActorRef,
          spotId: rid('room'),
          spotGeneration: 9n,
          membershipEpoch: 12n
        };
      },
      status() {
        return { routingId: rid('target-node'), lifecycleGeneration: 4n };
      }
    }),
    async notifyEntrySpotActorLeft() {},
    locationLifecycle: () => lifecycle,
    authorityStore: () => undefined,
    actorHandoff: {},
    actorTransferRegistry: {},
    relocationStore: () => ({
      async read() { return foundBlob(acceptedJournal); }
    }),
    shutdownSignal: () => shutdown.signal,
    clearRemoteActorPacketTarget() {}
  });

  await assert.rejects(
    async () => {
      await runtime.claimRoutedActorLocation(actor, rid('room'), 'play');
      await runtime.publishRoutedActorOwnership(actor);
    },
    /stopped before command 45 ACK/
  );
  assert.equal(released, 0);
  assert.equal(ownsLocation, true);
  assert.equal(locationGeneration, 17n);
});

test('transferred actor commit does not duplicate the native session binding restored by Core', () => {
  const actor = { context: { actorId: 'alice' } };
  const actorRef = { nodeRid: rid('target-node'), actorId: 'alice', generation: 9n };
  const sessionNodeRid = rid('session-node');
  const sessionRid = rid('session-rid');
  const binds = [];
  const state = {
    nativeActorRef: actorRef,
    remoteBoundSessionTarget: {
      routerChannelId: 'session.route',
      targetNodeRid: sessionNodeRid,
      spotId: sessionNodeRid,
      sessionNodeRid,
      sessionRid
    },
    setJoinedSpot(spotId, spot) {
      this.spotId = spotId;
      this.spot = spot;
    }
  };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {},
    spotManager: () => undefined,
    actorManager: () => ({ getState: () => state }),
    primarySpotNode: () => ({
      bindRemoteActorSession(boundActor, boundNodeRid, boundSessionRid) {
        binds.push({ boundActor, boundNodeRid, boundSessionRid });
      }
    }),
    async notifyEntrySpotActorLeft() {},
    locationLifecycle: () => undefined,
    actorHandoff: {},
    actorTransferRegistry: {},
    clearRemoteActorPacketTarget() {}
  });

  runtime.commitRoutedActor(actor, rid('room'), { name: 'room' });

  assert.equal(binds.length, 0);
  assert.equal(state.spotId.toHex(), rid('room').toHex());
  assert.deepEqual(state.spot, { name: 'room' });
});

test('native actor join rollback restores Entry location without destroying the existing actor', async () => {
  const actor = { context: { actorId: 'alice' } };
  let restoredToEntry = 0;
  let destroyed = 0;
  let joined = true;
  let ownsLocation = true;
  let restoredLocation;
  const state = {
    actorType: 'player',
    get ownsLocation() { return ownsLocation; },
    clearJoinedSpot() { joined = false; },
    markLocationReleased() { ownsLocation = false; }
  };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {},
    spotManager: () => undefined,
    actorManager: () => ({
      getState: () => state,
      async rollbackTransferredActor() { destroyed++; }
    }),
    primarySpotNode: () => { throw new Error('not used'); },
    async notifyEntrySpotActorLeft() {},
    locationLifecycle: () => ({
      async notifyActorLeftSpot(...args) {
        restoredToEntry++;
        restoredLocation = args;
      }
    }),
    actorHandoff: {},
    actorTransferRegistry: {},
    clearRemoteActorPacketTarget() {}
  });

  await runtime.rollbackNativeActorJoin(actor, {
    locationSpotId: rid('entry'),
    spotGeneration: 5n,
    membershipEpoch: 9n,
    ownerNodeGeneration: 3n
  });
  assert.equal(joined, false);
  assert.equal(restoredToEntry, 1);
  assert.equal(restoredLocation[2].toHex(), rid('entry').toHex());
  assert.deepEqual(restoredLocation.slice(3), [5n, 9n, 3n]);
  assert.equal(ownsLocation, true);
  assert.equal(destroyed, 0);
});

test('native actor join rollback restores the previous User SPOT location', async () => {
  const actor = { context: { actorId: 'alice' } };
  const previousSpot = { name: 'source-room' };
  const previousSpotId = rid('source-room');
  let currentSpotId = rid('target-room');
  let currentSpot = { name: 'target-room' };
  let restoredLocationRid;
  let generation = 7n;
  const state = {
    actorType: 'player',
    nativeActorRef: { nodeRid: rid('entry-node'), actorId: 'alice', generation: 4n },
    ownsLocation: true,
    setJoinedSpot(spotId, spot) { currentSpotId = spotId; currentSpot = spot; },
    setLocationGeneration(value) { generation = value; },
    clearAfterDestroy() {}
  };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {},
    spotManager: () => undefined,
    actorManager: () => ({ getState: () => state }),
    primarySpotNode: () => { throw new Error('not used'); },
    async notifyEntrySpotActorLeft() {},
    locationLifecycle: () => ({
      async takeoverActorJoinedSpot(_type, _id, _ref, meshName, spotId) {
        assert.equal(meshName, 'source-mesh');
        restoredLocationRid = spotId;
        return { status: 'claimed', generation: 8n };
      }
    }),
    actorHandoff: {},
    actorTransferRegistry: {},
    clearRemoteActorPacketTarget() {}
  });

  await runtime.rollbackNativeActorJoin(
    actor,
    {
      spotId: previousSpotId,
      spot: previousSpot,
      spotMeshName: 'source-mesh',
      actorRef: state.nativeActorRef,
      spotGeneration: 7n,
      membershipEpoch: 11n,
      ownerNodeGeneration: 3n
    }
  );
  assert.equal(currentSpotId.toHex(), previousSpotId.toHex());
  assert.equal(currentSpot, previousSpot);
  assert.equal(restoredLocationRid.toHex(), previousSpotId.toHex());
  assert.equal(generation, 8n);
});

test('Entry actor transaction keeps committed entry state when joined callback rejects', async () => {
  const actor = { context: { actorId: 'alice' } };
  const previousSpot = { name: 'room' };
  const previousRef = { nodeRid: rid('old-node'), actorId: 'alice', generation: 4n };
  let spotId = rid('room-node');
  let spot = previousSpot;
  let nativeActorRef = previousRef;
  let clearedTargets = 0;
  const state = {
    actor,
    get spotId() { return spotId; },
    get spot() { return spot; },
    get nativeActorRef() { return nativeActorRef; },
    clearJoinedSpot() { spotId = undefined; spot = undefined; },
    setJoinedSpot(value, target) { spotId = value; spot = target; },
    setNativeActorRef(value) { nativeActorRef = value; }
  };
  const runtime = new ZLinkEntryActorRuntimeService({
    actorManager: () => ({ getState: () => state }),
    spotManager: () => undefined,
    spotNodeRuntime: () => ({
      primaryMeshNode: {
        status: () => ({ routingId: rid('entry-node') })
      }
    }),
    streamBindingRuntime: { async refreshActor() {} },
    boundSessionRelay: { clearRemoteActorPacketTarget() { clearedTargets++; } }
  });

  await assert.rejects(
    () => runtime.commitActorTransaction(actor, async () => { throw new Error('joined failed'); }),
    /joined failed/
  );
  assert.equal(spotId, undefined);
  assert.equal(spot, undefined);
  assert.equal(nativeActorRef.nodeRid.toHex(), rid('entry-node').toHex());
  assert.equal(clearedTargets, 1);
});

test('ZLinkActorNativeJoinCoordinator joins entry spot and clears user spot state', async () => {
  const events = [];
  const entryTimeouts = [];
  const createdRef = { nodeRid: 'node-a', actorId: 'alice', generation: 1n };
  const entryRef = { nodeRid: 'node-b', actorId: 'alice', generation: 4n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup() {
      return undefined;
    },
    createActor() {
      return createdRef;
    },
    joinActorEntrySpot(actorRef, nodeRid, request, callback, timeoutMs) {
      // Deferred Join은 절대 deadline을 유지하므로 남은 시간이 전달된다.
      entryTimeouts.push(timeoutMs);
      events.push(`joinEntry:${actorRef.generation}:${nodeRid}:${request.data().toString()}`);
      callback({
        result: 0,
        joinResultCode: 0,
        actor: entryRef,
        targetNodeRid: nodeRid,
        joinedSpotId: nodeRid,
        joinEpoch: 5n,
        flags: 0
      }, [zlink.Message.from('entry-ok')]);
      return true;
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      completionTableProvider: () => node.completionTable
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  manager.getState('alice').setJoinedSpot('stage-1');

  const entryRequest = encodedMessage('entry');
  const result = await submitDeferredActorJoin(actor, actor.context.joinEntrySpot(entryRequest).timeout(50));

  assert.deepEqual(result.actor, {
    actorId: entryRef.actorId,
    objectGeneration: entryRef.generation,
    meshName: 'play',
    nodeRid: 'node-b'
  });
  // isJoined는 runtime state가 소유한다. Context 계약은 spotId만 노출한다.
  assert.equal(manager.getState('alice').isJoined, false);
  assert.equal(actor.context.spotId, undefined);
  assert.equal(manager.getState('alice').nativeActorRef, entryRef);
  assert.deepEqual(events, ['joinEntry:1:node-a:entry']);
  assert.equal(entryTimeouts.length, 1);
  assert.ok(entryTimeouts[0] > 0 && entryTimeouts[0] <= 50,
    `entry join timeout ${entryTimeouts[0]} must be within (0, 50]`);
});

test('ZLinkActorNativeJoinCoordinator uses formal transfer when replacement process owns the Entry route', async () => {
  const events = [];
  const sourceRef = { nodeRid: rid('node-source'), actorId: 'alice', generation: 1n };
  const entryRef = { nodeRid: rid('node-entry'), actorId: 'alice', generation: 2n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      return new PlayerActor(context.actorId, context);
    }
  }
  const node = createMockSpotNode({
    // The replacement process is already the current Entry route, but the
    // ActorRef still identifies the source process until formal transfer.
    routingId: rid('node-entry'),
    createActor() {
      return sourceRef;
    },
    joinActorEntrySpot(actorRef, nodeRid, request, callback) {
      const wire = JSON.parse(request.getString('utf8'));
      assert.equal(wire.actorType, 'player');
      assert.equal(wire.sourceSpotId, 'room-1');
      assert.equal(wire.spotId, 'node-entry');
      assert.equal(typeof wire.transferId, 'string');
      if (wire.phase === 'admission') {
        assert.equal(wire.transferState, undefined);
      } else {
        assert.equal(wire.phase, 'commit');
        assert.equal(typeof wire.transferState, 'string');
      }
      events.push(`joinEntry:${String(nodeRid)}:${wire.phase}:${wire.sourceSpotId}`);
      callback({
        result: 0,
        joinResultCode: 0,
        actor: { ...actorRef, nodeRid, generation: entryRef.generation },
        joinedSpotId: nodeRid,
        joinEpoch: 8n
      }, [zlink.Message.from('entry-ok')]);
      return true;
    }
  });
  const sourceTerminalOperation = { high: 0n, low: 900n };
  const originalWait = node.completionTable.wait;
  node.requestToNode = (_targetNodeRid, payload) => {
    const terminal = JSON.parse(Buffer.from(payload).toString());
    events.push(`sourceTerminal:${terminal.succeeded}`);
    return sourceTerminalOperation;
  };
  node.completionTable.wait = async (operationId, signal) => {
    if (operationId.low === sourceTerminalOperation.low) {
      return { terminalResult: 0, failureErrno: 0, operationKind: 7, kindData: null, parts: [] };
    }
    return await originalWait(operationId, signal);
  };
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      entrySpotIdProvider: () => 'node-entry',
      completionTableProvider: () => node.completionTable,
      spotRouteResolver: {
        async resolve(spotId) {
          events.push(`resolve:${String(spotId)}`);
          return {
            routerChannelId: 'play.route',
            targetNodeRid: rid('node-entry'),
            spotId,
            spotKind: framework.ZLinkSpotKind.Entry,
            targetSpotGeneration: 7n,
            targetNodeGeneration: 3n,
            authorityOwnerGeneration: 4n,
            targetOwnerId: 'entry-owner',
            ownerLeaseGeneration: 5n,
            authorityStoreVersion: 'entry:7'
          };
        }
      },
      sourceTransfer: {
        async prepareSource(_actor, state, _signal, authority) {
          events.push(`prepare:${authority}`);
          state.beginMove();
          return {
            state: framework.ZLinkMessage.from({ version: 1 }),
            handoffBacklog: [],
            async reserveTarget() {
              events.push('reserveTarget');
            },
            async commitAuthority() {
              events.push('commitAuthority');
            },
            commit() {
              events.push('commit');
              state.endMove();
            },
            async rollback() {
              events.push('rollback');
              state.endMove();
            }
          };
        }
      },
      async remoteActorBinder(actorRef) {
        events.push(`bind:${String(actorRef.nodeRid)}`);
      },
      remoteActivationWaiter: async (_actorId, targetNodeRid) => {
        events.push(`targetReady:${String(targetNodeRid)}`);
      }
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');
  manager.getState('alice').setJoinedSpot(rid('room-1'), undefined, 3n, 6n);
  // The cached Entry owner may be stale after rolling replacement. The
  // resolver must win over the old node passed by the lifecycle bridge.
  manager.getState('alice').setEntryNodeRid(rid('stale-entry'));

  const accepted = await actor.context[framework.ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME](
    rid('stale-entry'),
    encodedMessage('entry')
  );

  assert.equal(accepted, true);
  assert.deepEqual(events, [
    'resolve:node-entry',
    'joinEntry:node-entry:admission:room-1',
    'prepare:core',
    'reserveTarget',
    'joinEntry:node-entry:commit:room-1',
    'commitAuthority',
    'sourceTerminal:true',
    'commit',
    'bind:node-entry'
  ]);
  assert.equal(manager.getState('alice').isMoving, false);
  assert.equal(manager.getState('alice').isJoined, false);
});

test('DefaultZLinkActorManager destroys only entry-owned actors and ignores stale instances', async () => {
  const events = [];
  const createdRef = { nodeRid: 'node-a', actorId: 'alice', generation: 1n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    createActor(actorId) {
      events.push(`createNative:${actorId}`);
      return { ...createdRef, actorId };
    },
    async destroyActor(actorRef) {
      events.push(`destroyNative:${actorRef.actorId}:${actorRef.generation}`);
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    actorDestroyedCleanup(actorId) {
      events.push(`cleanup:${actorId}`);
    }
  });

  const actor = await manager.getOrCreateActor('alice', 'player');
  manager.getState('alice').ensureNativeActorRef(node);
  manager.getState('alice').setJoinedSpot('stage-1');

  await assert.rejects(
    () => manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor),
    { kind: framework.ZLinkFrameworkErrorKind.NotFound }
  );
  assert.equal(await manager.findActor('alice'), actor);

  manager.getState('alice').clearJoinedSpot();
  await manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor);
  await manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor);
  assert.equal(await manager.find('alice'), undefined);
  assert.equal(await manager.find('alice'), undefined);

  const recreated = await manager.getOrCreateActor('alice', 'player');
  manager.getState('alice').ensureNativeActorRef(node);
  await manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor);
  assert.equal(await manager.findActor('alice'), recreated);

  assert.deepEqual(events, [
    'createNative:alice',
    'destroyNative:alice:1',
    'cleanup:alice',
    'createNative:alice'
  ]);
});

test('DefaultZLinkActorManager shares concurrent destroy completion', async () => {
  let releaseDestroy;
  let destroyStarted;
  const started = new Promise((resolve) => { destroyStarted = resolve; });
  const release = new Promise((resolve) => { releaseDestroy = resolve; });
  let nativeDestroyCalls = 0;
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return { actorId, context };
    }
  }
  const node = createMockSpotNode({
    createActor(actorId) {
      return { nodeRid: 'node-a', actorId, generation: 1n };
    },
    async destroyActor() {
      nativeDestroyCalls += 1;
      destroyStarted();
      await release;
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: node
  });
  const actor = await manager.getOrCreateActor('alice', 'player');

  const first = manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor);
  await started;
  let secondCompleted = false;
  const second = manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor)
    .then(() => { secondCompleted = true; });
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(secondCompleted, false);
  assert.equal(nativeDestroyCalls, 1);
  releaseDestroy();
  await Promise.all([first, second]);
  assert.equal(await manager.findActor('alice'), undefined);
});

test('DefaultZLinkActorManager retries cleanup without destroying the native actor twice', async () => {
  let nativeDestroyCalls = 0;
  let releaseCalls = 0;
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return { actorId, context };
    }
  }
  const node = createMockSpotNode({
    createActor(actorId) {
      return { nodeRid: 'node-a', actorId, generation: 1n };
    },
    async destroyActor() {
      nativeDestroyCalls += 1;
    }
  });
  const options = {
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: node
  };
  const manager = createActorManager(options);
  const actor = await manager.getOrCreateActor('alice', 'player');
  manager.getState('alice').markLocationOwned();
  options.locationLifecycle = {
    async releaseActor() {
      releaseCalls += 1;
      if (releaseCalls === 1) throw new Error('location release failed');
    }
  };

  await assert.rejects(
    () => manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor),
    /location release failed/
  );
  assert.equal(manager.getState('alice').nativeActorRef, undefined);

  await manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor);
  assert.equal(nativeDestroyCalls, 1);
  assert.equal(releaseCalls, 2);
  assert.equal(await manager.findActor('alice'), undefined);
});

test('DefaultZLinkActorManager adopts native actor ref before creating routed actor instance', async () => {
  const events = [];
  const targetRef = { nodeRid: 'node-a', actorId: 'alice', generation: 9n };
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      events.push(`createApp:${actorId}`);
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup(actorId) {
      events.push(`lookupNative:${actorId}`);
      return undefined;
    },
    createActor(actorId) {
      events.push(`createNative:${actorId}`);
      throw new Error('native actor must already be owned by core join admission');
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNodeProvider: () => node
  });

  const actor = await manager.getOrCreateWithNativeRef('alice', 'player', targetRef);
  const again = await manager.getOrCreateActor('alice', 'player');

  assert.equal(again, actor);
  assert.deepEqual(manager.getState('alice').nativeActorRef, targetRef);
  assert.deepEqual(events, ['createApp:alice']);
});

test('DefaultZLinkActorManager adopts remote native actor ref without claiming actor location', async () => {
  const store = new framework.ZLinkInMemoryLocationStore(() => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)));
  const owner = await locationLifecycleNode(store, 'owner-a', 'node-a');
  const remote = await locationLifecycleNode(store, 'owner-b', 'node-b');

  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return { actorId, context };
    }
  }

  const ownerNode = createMockSpotNode({
    routingId: rid('node-a'),
    createActor(actorId) {
      return { nodeRid: rid('node-a'), actorId, generation: 1n };
    }
  });
  const ownerManager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNode: ownerNode,
    locationLifecycle: owner.lifecycle
  });

  await ownerManager.create('alice', 'player').inMesh('play').submit();
  const ownedRow = await store.resolveActor({ meshName: 'play', actorId: 'alice' });
  assert.equal(ownedRow.ownerId, 'owner-a');

  const remoteManager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    nativeActorNodeProvider: () => createMockSpotNode({ routingId: rid('node-b') }),
    locationLifecycle: remote.lifecycle
  });
  const actor = await remoteManager.getOrCreateWithNativeRef(
    'alice',
    'player',
    { nodeRid: rid('node-a'), actorId: 'alice', generation: 1n }
  );

  assert.equal(actor.actorId, 'alice');
  const rowAfterRemoteAdoption = await store.resolveActor({ meshName: 'play', actorId: 'alice' });
  assert.equal(rowAfterRemoteAdoption.ownerId, 'owner-a');
  assert.equal(remoteManager.getState('alice').ownsLocation, false);
});

test('DefaultZLinkActorManager runs destroy cleanup for local actors without native refs', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    async destroyActor(actorRef) {
      events.push(`destroyNative:${actorRef.actorId}`);
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    actorDestroyedCleanup(actorId) {
      events.push(`cleanup:${actorId}`);
    }
  });

  const actor = await manager.getOrCreateActor('local-alice', 'player');
  await manager.destroyActor(node, zlink.RoutingId.from('node-a'), actor);

  assert.equal(await manager.find('local-alice'), undefined);
  assert.deepEqual(events, ['cleanup:local-alice']);
});

test('ZLinkEntrySpotActivation destroyActor does not invoke Entry Spot lifecycle callbacks', async () => {
  const events = [];
  class EntrySpot {
    async onCreateActor(actor) {
      events.push(`entryCreate:${actor.context.actorId}`);
    }
    async onLeaveActor(actor) {
      events.push(`entryLeave:${actor.actorId}`);
    }
  }
  const nativeSpot = {
    routingId: 'entry-stage',
    async dispose() {}
  };
  const activation = new framework.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot,
    nativeNode: { routingId: 'node-a' },
    nodeRid: 'node-a',
    spotNodeName: 'node-a',
    entryActorRuntime: createEntryActorRuntime(
      () => undefined,
      async (_node, nodeRid, actor) => {
        events.push(`destroyHook:${nodeRid}:${actor.context.actorId}`);
      }
    )
  });

  await activation.create();
  await activation.notifyCreateActor(lifecycleActor('alice'), framework.ZLinkMessage.from({}));
  await activation.context.destroyActor(lifecycleActor('alice'));

  assert.deepEqual(events, [
    'entryCreate:alice',
    'destroyHook:node-a:alice'
  ]);
});

test('ZLinkEntrySpotActivation disposes native resources when onClosing fails', async () => {
  const events = [];
  class EntrySpot {
    async onClosing() {
      events.push('closing');
      throw new Error('entry close failed');
    }
  }
  const activation = new framework.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot: {
      routingId: 'entry-stage',
      async dispose() { events.push('native-dispose'); }
    },
    nativeNode: { routingId: 'node-a' },
    nodeRid: 'node-a',
    spotNodeName: 'node-a'
  });

  await activation.create();
  await activation.initialize();
  await assert.rejects(() => activation.dispose(), /entry close failed/);
  await activation.dispose();
  assert.deepEqual(events, ['closing', 'native-dispose']);
});

test('Entry actor commit keeps accepted state while stream binding retries post-commit', async () => {
  const actor = { context: { actorId: 'alice', meshName: 'play' } };
  let attempts = 0;
  let joinedSpotCleared = false;
  let installedRef;
  let remoteTargetCleared = false;
  let resolveRefreshed;
  const refreshed = new Promise((resolve) => { resolveRefreshed = resolve; });
  const state = {
    actor,
    meshName: 'play',
    nativeActorRef: { nodeRid: rid('old-node'), actorId: 'alice', generation: 4n },
    clearJoinedSpot() { joinedSpotCleared = true; },
    setNativeActorRef(actorRef) { installedRef = actorRef; }
  };
  const runtime = new ZLinkEntryActorRuntimeService({
    actorManager: () => ({ getState: () => state }),
    spotManager: () => undefined,
    spotNodeRuntime: () => ({
      primaryMeshNode: {
        status: () => ({ routingId: rid('entry-node') })
      }
    }),
    streamBindingRuntime: {
      async refreshActor(actorRef) {
        attempts++;
        if (attempts === 1) throw new Error('binding temporarily unavailable');
        resolveRefreshed(actorRef);
      }
    },
    boundSessionRelay: {
      clearRemoteActorPacketTarget(actorId) {
        remoteTargetCleared = actorId === 'alice';
      }
    }
  });

  await runtime.commitActorTransaction(actor, async () => {});
  const refreshedRef = await Promise.race([
    refreshed,
    new Promise((_, reject) => setTimeout(() => reject(new Error('binding retry timed out')), 500))
  ]);
  assert.equal(attempts, 2);
  assert.equal(joinedSpotCleared, true);
  assert.equal(remoteTargetCleared, true);
  assert.equal(String(installedRef.nodeRid), String(rid('entry-node')));
  assert.equal(refreshedRef.actorId, installedRef.actorId);
  assert.equal(refreshedRef.objectGeneration, installedRef.generation);
  assert.equal(refreshedRef.meshName, 'play');
  assert.equal(String(refreshedRef.nodeRid), String(installedRef.nodeRid));
});

// ActorJoinReadable dispatch-event value (core SpotDispatchEvent.ActorJoinReadable = 6).
const ENTRY_ACTOR_JOIN_READABLE = 6;

test('native actor join admission closes caller-owned reply when submit fails', async () => {
  const requestMessage = zlink.Message.from(Buffer.from('join-request'));
  const originalClose = ZLinkBufferMessage.prototype.close;
  let replyMessage;
  let replyCloseCount = 0;
  ZLinkBufferMessage.prototype.close = function closeTrackedMessage() {
    if (this === replyMessage) {
      replyCloseCount += 1;
    }
    return originalClose.call(this);
  };

  try {
    const admission = new ZLinkSpotNativeActorJoinAdmission({
      nativeSpot: {
        replyActorJoin() {
          return {
            message(message) {
              replyMessage = message;
              return this;
            },
            submit() {
              throw new Error('actor join reply submit failed');
            }
          };
        }
      },
      serial: {
        execute(action) {
          return Promise.resolve().then(action);
        }
      },
      resolveActor: () => lifecycleActor('joined-actor'),
      getTarget: () => ({
        async onActorJoin() {
          return { accepted: true, reply: 'accepted' };
        }
      }),
      defaultAccept: false
    });

    await assert.rejects(
      admission.admit({
        info: {
          targetActor: { nodeRid: rid('node-a'), actorId: 'joined-actor', generation: 1n },
          joinEpoch: 1n
        },
        message: requestMessage
      }),
      /actor join reply submit failed/
    );
    assert.notEqual(replyMessage, undefined);
    assert.equal(replyCloseCount, 1);
  } finally {
    ZLinkBufferMessage.prototype.close = originalClose;
    requestMessage.close();
  }
});

test('native actor join skips Entry admission and commits membership after the native reply', async () => {
  const events = [];
  const actor = lifecycleActor('alice');
  const request = zlink.Message.from('return-home');
  const admission = new ZLinkSpotNativeActorJoinAdmission({
    nativeSpot: {
      replyActorJoin(_request, code) {
        assert.equal(code, 0);
        return {
          submit() { events.push('native-reply'); }
        };
      }
    },
    serial: {
      execute(action) { return Promise.resolve().then(action); }
    },
    resolveActor: () => actor,
    getTarget: () => ({
      async onActorJoin() {
        events.push('admit');
        return { accepted: true };
      }
    }),
    defaultAccept: true,
    async commitAcceptedActor(committed) {
      assert.equal(committed, actor);
      events.push('entry-joined');
    }
  });

  await admission.admit({
    info: {
      targetActor: { nodeRid: rid('node-a'), actorId: actor.actorId, generation: 1n },
      joinEpoch: 1n
    },
    message: request
  });

  assert.deepEqual(events, ['native-reply', 'entry-joined']);
  request.close();
});

// Drives the native recv -> admit -> reply round-trip the Entry Spot activation
// registers via setDispatchHandler. This mirrors how core delivers an admission
// request to the target node (local or remote), so the test exercises the same
// server-side admission path used in production rather than a caller-local shim.
function createEntryJoinHarness() {
  let dispatchHandler;
  const queue = [];
  const replies = [];
  let pending = 0;
  let resolveDone;
  const nativeSpot = {
    routingId: 'entry-stage',
    setDispatchHandler(handler) {
      dispatchHandler = handler;
    },
    recvActorJoin() {
      return queue.shift() ?? null;
    },
    replyActorJoin(request, code) {
      let replyMessage;
      return {
        message(message) {
          replyMessage = message;
          return this;
        },
        submit() {
          replies.push({ actorId: request.info.targetActor.actorId, code, reply: replyMessage });
          pending -= 1;
          if (pending === 0 && resolveDone !== undefined) {
            resolveDone();
          }
        }
      };
    },
    async dispose() {}
  };
  return {
    nativeSpot,
    enqueue(actorId, message) {
      queue.push({
        info: {
          targetActor: { nodeRid: rid('node-a'), actorId, generation: 1n },
          joinEpoch: 1n
        },
        message
      });
      pending += 1;
    },
    async run() {
      const done = new Promise((resolve) => {
        resolveDone = pending === 0 ? resolve() : resolve;
      });
      dispatchHandler({ event: ENTRY_ACTOR_JOIN_READABLE });
      await done;
      // The native reply commits before the Entry membership callback. Wait for
      // the detached drain to finish instead of treating reply submission as
      // lifecycle completion.
      await new Promise((resolve) => setTimeout(resolve, 10));
    },
    replies
  };
}

function createEntryActorRuntime(resolveActor, destroyActor = async () => {}) {
  return {
    resolveActor,
    async commitActorTransaction(_actor, onJoined) { await onJoined(); },
    destroyActor,
    async routePacket() {
      return { handled: false };
    }
  };
}

test('ZLinkEntrySpotActivation does not expose Entry admission on the native dispatch round-trip', async () => {
  const events = [];
  class EntrySpot {
    async onActorJoin(actorId, request) {
      const reason = request.decode();
      events.push(`entryJoin:${actorId}:${reason}`);
      return reason === 'blocked'
        ? { accepted: false, reply: 'entry-reject-reply' }
        : { accepted: true, reply: 'entry-accept-reply' };
    }
    async onJoinedActor(actor) {
      events.push(`entryJoined:${actor.context.actorId}`);
    }
  }
  const harness = createEntryJoinHarness();
  const activation = new framework.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot: harness.nativeSpot,
    nativeNode: { routingId: 'node-a', bindRemoteActorSession() {} },
    nodeRid: 'node-a',
    spotNodeName: 'node-a',
    entryActorRuntime: createEntryActorRuntime((actorId) => lifecycleActor(actorId))
  });
  await activation.create();
  await activation.initialize();

  const acceptRequest = zlink.Message.from('return-to-entry');
  const rejectRequest = zlink.Message.from('blocked');
  harness.enqueue('alice', acceptRequest);
  harness.enqueue('bob', rejectRequest);
  await harness.run();

  assert.equal(harness.replies[0].actorId, 'alice');
  assert.equal(harness.replies[0].code, 0);
  assert.equal(harness.replies[0].reply, undefined);
  assert.equal(harness.replies[1].actorId, 'bob');
  assert.equal(harness.replies[1].code, 0);
  assert.equal(harness.replies[1].reply, undefined);
  assert.deepEqual(events, [
    'entryJoined:alice',
    'entryJoined:bob'
  ]);
  acceptRequest.close();
  rejectRequest.close();
});

test('native actor join remains accepted when post-commit joined callback throws', async () => {
  class EntrySpot {
    async onActorJoin() { return { accepted: true }; }
    async onJoinedActor() { throw new Error('joined failed'); }
  }
  const harness = createEntryJoinHarness();
  const activation = new framework.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot: harness.nativeSpot,
    nativeNode: { routingId: 'node-a', bindRemoteActorSession() {} },
    nodeRid: 'node-a',
    spotNodeName: 'node-a',
    entryActorRuntime: createEntryActorRuntime((actorId) => lifecycleActor(actorId))
  });
  await activation.create();
  await activation.initialize();

  const request = zlink.Message.from('join');
  harness.enqueue('alice', request);
  await harness.run();

  assert.equal(harness.replies[0].code, 0);
  request.close();
  await activation.dispose();
});

test('ZLinkEntrySpotActivation auto-accepts dispatched entry join when onActorJoin is absent', async () => {
  const events = [];
  class EntrySpot {
    async onJoinedActor(actor) {
      events.push(`entryJoined:${actor.context.actorId}`);
    }
  }
  const harness = createEntryJoinHarness();
  const activation = new framework.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot: harness.nativeSpot,
    nativeNode: { routingId: 'node-a', bindRemoteActorSession() {} },
    nodeRid: 'node-a',
    spotNodeName: 'node-a',
    entryActorRuntime: createEntryActorRuntime((actorId) => lifecycleActor(actorId))
  });
  await activation.create();
  await activation.initialize();

  const request = zlink.Message.from('return-to-entry');
  harness.enqueue('alice', request);
  await harness.run();

  assert.deepEqual(harness.replies, [{ actorId: 'alice', code: 0, reply: undefined }]);
  assert.deepEqual(events, ['entryJoined:alice']);
  request.close();
});

test('ZLinkEntrySpotActivation rejects dispatched entry join when actor is unknown', async () => {
  const events = [];
  class EntrySpot {
    async onActorJoin(actorId) {
      events.push(`entryJoin:${actorId}`);
      return { accepted: true };
    }
  }
  const harness = createEntryJoinHarness();
  const activation = new framework.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot: harness.nativeSpot,
    nativeNode: { routingId: 'node-a', bindRemoteActorSession() {} },
    nodeRid: 'node-a',
    spotNodeName: 'node-a',
    entryActorRuntime: createEntryActorRuntime(() => undefined)
  });
  await activation.create();
  await activation.initialize();

  const request = zlink.Message.from('return-to-entry');
  harness.enqueue('ghost', request);
  await harness.run();

  assert.deepEqual(harness.replies, [{ actorId: 'ghost', code: 1, reply: undefined }]);
  assert.deepEqual(events, []);
  request.close();
});

test('ZLinkActorNativeJoinCoordinator maps native join failures to framework errors', async () => {
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    create(context) {
      const { actorId } = context;
      return new PlayerActor(actorId, context);
    }
  }
  const node = createMockSpotNode({
    actorLookup() {
      return { nodeRid: 'node-a', actorId: 'alice', generation: 1n };
    },
    joinActor(actorRef, targetNodeRid, targetSpotId, payload, callback) {
      callback({
        result: 109,
        joinResultCode: 0,
        actor: actorRef,
        joinedSpotId: targetSpotId,
        joinEpoch: 0n,
        flags: 0
      }, []);
      return true;
    }
  });
  const manager = createActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
      node,
      completionTableProvider: () => node.completionTable,
      spotRouteResolver: {
        async resolve(spotId) {
          return {
            routerChannelId: 'play',
            targetNodeRid: rid('node-a'),
            spotId: rid(String(spotId)),
            spotKind: framework.ZLinkSpotKind.User,
            targetSpotGeneration: 9n
          };
        }
      }
    })
  });
  const actor = await manager.getOrCreateActor('alice', 'player');

  await assert.rejects(
    () => submitDeferredActorJoin(actor, actor.context.joinSpot('stage-1', 'hello')),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
});

test('ZLinkSpotActorDispatcher invokes send request and lifecycle handlers without fallback', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class MoveSendHandler {
    async handle(spot, actor, context, message) {
      assert.equal(spot.name, 'game');
      events.push(`send:${actor.context.actorId}:${context.packetName}:${message}`);
    }
  }
  class MoveRequestHandler {
    async handle(spot, actor, context, request) {
      assert.equal(spot.name, 'game');
      events.push(`request:${actor.context.actorId}:${context.packetName}:${request}`);
      return 'ok';
    }
  }
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime()
    .addPacket({
      kind: framework.ZLinkActorPacketKind.Send,
      packetName: 'move',
      actorType: PlayerActor,
      handlerType: MoveSendHandler
    })
    .addPacket({
      kind: framework.ZLinkActorPacketKind.Request,
      packetName: 'move',
      actorType: PlayerActor,
      handlerType: MoveRequestHandler
    });
  const actor = new PlayerActor('alice', lifecycleContext('alice'));
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry,
    spot: {
      name: 'game',
      async onJoinedActor(joinedActor) {
        events.push(`joined:game:${joinedActor.context.actorId}`);
      },
      async onLeaveActor(leftActor) {
        events.push(`left:game:${leftActor.context.actorId}`);
      },
      async onDisconnectActor(disconnectedActor) {
        events.push(`disconnected:game:${disconnectedActor.context.actorId}`);
      }
    }
  });

  await dispatcher.dispatchSend(actor, 'move', 'left');
  const reply = await dispatcher.dispatchRequest(actor, 'move', 'right');
  await dispatcher.notifyJoinActor(actor);
  await dispatcher.notifyLeaveActor(actor);
  await dispatcher.notifyDisconnectActor(actor);

  assert.equal(reply, 'ok');
  assert.deepEqual(events, [
    'send:alice:move:left',
    'request:alice:move:right',
    'joined:game:alice',
    'left:game:alice',
    'disconnected:game:alice'
  ]);
  await assert.rejects(
    () => dispatcher.dispatchRequest(actor, 'missing', 'payload'),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
});

test('ZLinkSpotActorDispatcher retains handler instances per actor activation', async () => {
  let creates = 0;
  let singletonGets = 0;
  class PlayerActor {
    constructor(actorId) {
      this.context = lifecycleContext(actorId);
    }
  }
  class MoveHandler {
    constructor() {
      this.id = ++creates;
    }

    async handle(_spot, _actor, _context, message) {
      return `${this.id}:${message}`;
    }
  }
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime()
    .addPacket({
      kind: framework.ZLinkActorPacketKind.Request,
      packetName: 'move',
      actorType: PlayerActor,
      handlerType: MoveHandler
    });
  const singleton = new MoveHandler();
  creates = 0;
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry,
    spot: {},
    providerResolver: {
      get() {
        singletonGets += 1;
        return singleton;
      },
      create(type) {
        return new type();
      }
    }
  });
  const alice = new PlayerActor('alice');
  const bob = new PlayerActor('bob');

  assert.equal(await dispatcher.dispatchRequest(alice, 'move', 'one'), '1:one');
  assert.equal(await dispatcher.dispatchRequest(alice, 'move', 'two'), '1:two');
  assert.equal(await dispatcher.dispatchRequest(bob, 'move', 'one'), '2:one');
  assert.equal(creates, 2);
  assert.equal(singletonGets, 0);
});

test('spot actor dispatch rejects malformed JSON as PayloadDecodeFailed before invoking the handler', async () => {
  const errors = [];
  let handlerInvocations = 0;
  class PlayerActor {
    constructor(actorId) {
      this.actorId = actorId;
    }
  }
  class BrokenPayloadHandler {
    async handle() {
      handlerInvocations += 1;
    }
  }
  const actor = new PlayerActor('alice');
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime().addPacket({
    kind: framework.ZLinkActorPacketKind.Send,
    packetName: 'BrokenPayload',
    actorType: PlayerActor,
    handlerType: BrokenPayloadHandler
  });
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: { context: { meshName: 'play' } },
    spotId: () => 'room-1',
    registry,
    resolveActor: () => actor,
    onDisconnectActor: async () => {},
    dispatchErrors: {
      flow: {
        accepts: () => false,
        flowCreationEnabled: () => false
      },
      report(error) {
        errors.push(error);
      }
    }
  });
  const header = zlink.Message.from(Buffer.from(framework.encodeStreamHeader({
    kind: framework.ZLinkStreamMessageKind.Send,
    codec: framework.ZLinkStreamCodec.Json,
    flags: framework.ZLinkStreamHeaderFlags.None,
    name: 'BrokenPayload',
    metadata: new Map()
  })));

  await assert.rejects(
    () => dispatch.dispatch('alice', [header, zlink.Message.from(Buffer.from('{'))]),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError
  );
  assert.equal(handlerInvocations, 0);
  assert.equal(errors.length, 1);
  assert.equal(errors[0].reason, 'decode_error');
  assert.equal(errors[0].action, 'drop');
});

test('spot actor dispatch rejects a missing handler before payload deserialization', async () => {
  let deserializeCalls = 0;
  class PlayerActor {
    constructor(actorId) {
      this.actorId = actorId;
    }
  }
  const actor = new PlayerActor('alice');
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: { context: { meshName: 'play' } },
    spotId: () => 'room-1',
    registry: new framework.ZLinkSpotActorHandlerRegistryRuntime(),
    resolveActor: () => actor,
    onDisconnectActor: async () => {},
    messageSerializers: new Map([['application/x-test', {
      serialize() {
        throw new Error('serialize must not be called');
      },
      deserialize() {
        deserializeCalls += 1;
        return { decoded: true };
      }
    }]])
  });
  const header = zlink.Message.from(Buffer.from(framework.encodeStreamHeader({
    kind: framework.ZLinkStreamMessageKind.Send,
    codec: framework.ZLinkStreamCodec.Json,
    flags: framework.ZLinkStreamHeaderFlags.None,
    name: 'MissingPacket',
    metadata: new Map()
  })));
  const payload = zlink.Message.from(Buffer.from('{not-json'));

  await assert.rejects(
    () => dispatch.dispatch('alice', [header, payload]),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
  assert.equal(deserializeCalls, 0);
  header.close();
  payload.close();
});

test('ZLinkSpotActorHandlerRegistryRuntime resolves actor packets registered without actor type', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId) {
      this.actorId = actorId;
    }
  }
  class MoveRequestHandler {
    async handle(_spot, actor, context, request) {
      events.push(`${actor.actorId}:${context.packetName}:${request}`);
      return 'ok';
    }
  }
  const actorHandlers = new framework.ZLinkSpotActorHandlerRegistryRuntime();
  const registry = new framework.DefaultZLinkSpotHandlerRegistry(actorHandlers);
  registry.actorRequest('move', MoveRequestHandler);

  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry: actorHandlers,
    spot: { name: 'game' }
  });

  assert.equal(await dispatcher.dispatchRequest(new PlayerActor('alice'), 'move', 'x'), 'ok');
  assert.deepEqual(events, ['alice:move:x']);
});

test('ZLinkSpotActorDispatcher commits actor join only when onActorJoin accepts', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  const actor = new PlayerActor('alice', lifecycleContext('alice'));
  let accept = true;
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry: new framework.ZLinkSpotActorHandlerRegistryRuntime(),
    spot: {
      async onActorJoin(actorId, request) {
        events.push(`join:${actorId}:${request.decode()}`);
        return accept
          ? { accepted: true, reply: 'accept-reply' }
          : { accepted: false, reply: 'reject-reply' };
      },
      async onJoinedActor(joinedActor) {
        events.push(`post:${joinedActor.context.actorId}`);
      }
    }
  });

  const acceptedRequest = zlink.Message.from('accept');
  const accepted = await dispatcher.admitActorJoin(actor, acceptedRequest, () => {
    events.push('commit:accept');
  });
  accept = false;
  const rejectedRequest = zlink.Message.from('reject');
  const rejected = await dispatcher.admitActorJoin(actor, rejectedRequest, () => {
    events.push('commit:reject');
  });

  assert.equal(accepted.accepted, true);
  assert.equal(accepted.reply, 'accept-reply');
  assert.equal(rejected.accepted, false);
  assert.equal(rejected.reply, 'reject-reply');
  assert.deepEqual(events, [
    'join:alice:accept',
    'commit:accept',
    'post:alice',
    'join:alice:reject'
  ]);
  acceptedRequest.close();
  rejectedRequest.close();
});

test('ZLinkSpotActorDispatcher rejects actor join by default when onActorJoin is absent', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  const actor = new PlayerActor('alice', {});
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry: new framework.ZLinkSpotActorHandlerRegistryRuntime(),
    spot: {
      async onJoinedActor(joinedActor) {
        events.push(`post:${joinedActor.actorId}`);
      }
    }
  });

  const request = zlink.Message.from('join');
  const result = await dispatcher.admitActorJoin(actor, request, () => {
    events.push('commit');
  });

  assert.equal(result.accepted, false);
  assert.equal(result.reply, undefined);
  assert.deepEqual(events, []);
  request.close();
});

test('ZLinkSpotActorDispatcher does not fallback actor requests to send handlers', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class MoveSendHandler {
    async handle(_spot, actor, context, message) {
      events.push(`send:${actor.context.actorId}:${context.packetName}:${message}`);
    }
  }
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime()
    .addPacket({
      kind: framework.ZLinkActorPacketKind.Send,
      packetName: 'move',
      actorType: PlayerActor,
      handlerType: MoveSendHandler
    });
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry,
    spot: {}
  });
  const actor = new PlayerActor('alice', {});

  await assert.rejects(
    () => dispatcher.dispatchRequest(actor, 'move', 'right'),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
  assert.deepEqual(events, []);
});

test('ZLinkSpotActorDispatcher keeps reply transport options internal', async () => {
  let replyOptions;
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class MoveRequestHandler {
    async handle(_spot, _actor, context, request) {
      assert.equal(context.packetName, 'move');
      assert.equal('reply' in context, false);
      return { accepted: true };
    }
  }
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime()
    .addPacket({
      kind: framework.ZLinkActorPacketKind.Request,
      packetName: 'move',
      actorType: PlayerActor,
      handlerType: MoveRequestHandler
    });
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry,
    spot: {}
  });
  const actor = new PlayerActor('alice', {});

  const reply = await dispatcher.dispatchRequestThen(
    actor,
    'move',
    'trace-101',
    {},
    (value, options) => {
      replyOptions = options;
      return value;
    }
  );

  assert.deepEqual(reply, { accepted: true });
  assert.equal(replyOptions.compressPayload, false);
  assert.deepEqual([...replyOptions.metadata.entries()], []);
  assert.equal(framework.DefaultZLinkSpotActorReplyOptions, undefined);
});

test('ZLinkSpotActorDispatcher serializes user spot actor handlers on provided serial executor', async () => {
  const events = [];
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class MoveSendHandler {
    async handle(_spot, _actor, _context, message) {
      events.push(`handler:${message}`);
    }
  }
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime()
    .addPacket({
      kind: framework.ZLinkActorPacketKind.Send,
      packetName: 'move',
      actorType: PlayerActor,
      handlerType: MoveSendHandler
    });
  const serial = new framework.ZLinkSpotSerialExecutor();
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry,
    spot: {},
    serial
  });
  const actor = new PlayerActor('alice', {});

  const first = serial.execute(async () => {
    events.push('spot:start');
    await new Promise((resolve) => setTimeout(resolve, 5));
    events.push('spot:end');
  });
  const send = dispatcher.dispatchSend(actor, 'move', 'left');
  await Promise.all([first, send]);

  assert.deepEqual(events, ['spot:start', 'spot:end', 'handler:left']);
});

async function locationLifecycleNode(store, ownerId, nodeRid) {
  const runtime = new framework.ZLinkLocationRuntime({
    stores: {
      locationStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: store
    },
    ownerId,
    now: () => new Date(Date.UTC(2026, 6, 3, 0, 0, 0)),
    setTimer() {
      return 0;
    },
    clearTimer() {}
  });
  await runtime.start(rid(nodeRid));
  return {
    runtime,
    lifecycle: new framework.ZLinkLocationLifecycle(runtime, store, 'play')
  };
}

function rid(value) {
  return zlink.RoutingId.from(value);
}

function createMockSpotNode(overrides) {
  let nextOperation = 1n;
  const completions = new Map();
  const node = {
    routingId: 'node-a',
    setRoutingId() {},
    setRouterBind() {},
    setPubBind() {},
    attachDiscovery() {},
    connectPeer() {},
    disconnectPeer() {},
    createSpot() { throw new Error('not used'); },
    getOrCreateSpot() { throw new Error('not used'); },
    status() {
      return {
        routingId: this.routingId instanceof zlink.RoutingId
          ? this.routingId
          : rid(String(this.routingId)),
        lifecycleGeneration: 1n
      };
    },
    peers() { return []; },
    subjects() { return []; },
    entrySpot() { throw new Error('not used'); },
    createActor(actorId) {
      return { nodeRid: 'node-a', actorId, generation: 1n };
    },
    actorLookup() {
      return undefined;
    },
    joinActor() {
      throw new Error('not used');
    },
    joinActorEntrySpot() {
      throw new Error('not used');
    },
    leaveActor() {
      const operationId = { high: 0n, low: nextOperation++ };
      completions.set(operationId.low, {
        terminalResult: 0,
        failureErrno: 0,
        operationKind: 8,
        kindData: null,
        parts: []
      });
      return operationId;
    },
    destroyActor() {
      throw new Error('not used');
    },
    sendActorBoundSession() {
      throw new Error('not used');
    },
    closeActorBoundSession() {
      throw new Error('not used');
    },
    async dispose() {},
    nativeInstance: {},
    ...overrides
  };
  node.completionTable = {
    async wait(operationId) {
      const completion = completions.get(operationId.low);
      if (completion === undefined) throw new Error(`missing completion ${operationId.low}`);
      completions.delete(operationId.low);
      return completion;
    }
  };
  node.joinActorSpot ??= (actor, targetNodeRid, targetSpotId, _targetGeneration, request, timeoutMs) => {
    const operationId = { high: 0n, low: nextOperation++ };
    const submitted = node.joinActor(
      actor,
      targetNodeRid,
      targetSpotId,
      zlink.Message.from(request),
      (result, parts) => {
        completions.set(operationId.low, legacyJoinCompletion(result, parts, targetSpotId));
      },
      timeoutMs
    );
    if (!submitted) throw new Error('formal actor join submit failed');
    return operationId;
  };
  const legacyJoinActorEntrySpot = node.joinActorEntrySpot.bind(node);
  node.joinActorEntrySpot = (actor, targetNodeRid, request, timeoutMs) => {
    const operationId = { high: 0n, low: nextOperation++ };
    const submitted = legacyJoinActorEntrySpot(
      actor,
      targetNodeRid,
      zlink.Message.from(request),
      (result, parts) => {
        completions.set(operationId.low, legacyJoinCompletion(result, parts, result.joinedSpotId ?? null));
      },
      timeoutMs
    );
    if (!submitted) throw new Error('formal entry actor join submit failed');
    return operationId;
  };
  return node;
}

function legacyJoinCompletion(result, parts, spotId) {
  return {
    terminalResult: result.result,
    failureErrno: result.result === 0 ? 0 : 1,
    operationKind: 7,
    kindData: {
      kind: 'actorJoinCompletion',
      joinResult: result.joinResultCode === 0 || result.joinResultCode === 7 ? 0 : 1,
      actor: result.actor,
      location: {
        actor: result.actor,
        spotId,
        spotGeneration: 1n,
        membershipEpoch: result.joinEpoch ?? 1n
      }
    },
    parts
  };
}

function foundBlob(bytes) {
  const storeNow = new Date();
  return {
    kind: 'found',
    bytes: Buffer.from(bytes),
    expiresAt: new Date(storeNow.getTime() + 60_000),
    storeNow
  };
}

function missingBlob() {
  return {
    kind: 'missing',
    storeNow: new Date()
  };
}
