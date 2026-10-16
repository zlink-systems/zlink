'use strict';

// #1772: a new Actor is created only on a node whose Entry Spot type accepts the Actor type
// (Actor model §6.3). Descriptors publish `entryAccepted`, the requester filters on it, and a
// target that no longer has such an Entry refuses before the factory so the requester excludes
// that host run.

const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { RoutingId } = require('@zlink-systems/zlink');
const nestjs = require('../../packages/nestjs/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ZLinkActorPlacementCoordinator
} = require('../../packages/framework/dist/runtime/host/actor-placement-coordinator');
const {
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  ZLinkFrameworkInternalErrorKind,
  internalFrameworkErrorKind,
  internalFrameworkWireReply
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

const POLL_MS = 10;
const OPERATION_TIMEOUT_MS = 5_000;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function until(label, predicate) {
  const deadline = Date.now() + OPERATION_TIMEOUT_MS;
  for (;;) {
    const value = await predicate();
    if (value) return value;
    assert.ok(Date.now() < deadline, label);
    await sleep(POLL_MS);
  }
}

class Entry {
  constructor(context) {
    this.context = context;
  }
  async onCreateActor() {
    return { accepted: true };
  }
}

function newFactory(created) {
  return class Factory {
    async create(context) {
      created.push(context.actorId);
      return { actorId: context.actorId, context };
    }
  };
}

test('new Actor creation is limited to nodes whose Entry Spot accepts the Actor type', async () => {
  const provider = new ZLinkInMemoryProviderLocationStore();
  const createdOn = { entry: [], plain: [] };
  const apps = [];
  async function start(node, withEntry) {
    const builder = nestjs.zlinkFramework().addLocationStore(provider);
    builder.configureLocations().pollingIntervalMs(POLL_MS).routeCacheMaxAgeMs(0);
    const server = builder
      .addRouteMesh('play')
      .listen('tcp://127.0.0.1:0')
      .routingId(node)
      .objects()
      .server();
    if (withEntry) server.addEntrySpot(Entry, 'player');
    const created = createdOn[node];
    server
      .addActorFactory('player', newFactory(created), (f) => f.disableRelocation())
      .addActorFactory('npc', newFactory(created), (f) => f.disableRelocation());
    const registration = builder.build();
    class TestModule {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(registration)] })(TestModule);
    const app = await NestFactory.createApplicationContext(TestModule, {
      logger: false,
      abortOnError: false
    });
    apps.push(app);
    return app;
  }
  try {
    const entry = await start('entry', true);
    const plain = await start('plain', false);
    const query = plain.get(nestjs.ZLINK_LOCATION_RUNTIME_QUERY);
    const descriptors = await until('both descriptors are published', async () => {
      const items = (await query.listMeshNodeDescriptors('play')).items;
      return items.length === 2 &&
        plain.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('play').readyPeerCount >= 1
        ? new Map(items.map((d) => [String(d.rid), d]))
        : undefined;
    });
    const accepted = (node, type) =>
      descriptors.get(node).objectCapabilities.find((c) => c.stableType === type).entryAccepted;
    assert.equal(accepted('entry', 'player'), true);
    assert.equal(accepted('entry', 'npc'), false);
    assert.equal(accepted('plain', 'player'), false);
    assert.equal(accepted('plain', 'npc'), false);

    // The requester has no Entry itself: the Actor is created on the node that has one.
    const actors = plain.get(nestjs.ZLINK_ACTOR_MANAGER);
    const result = await actors
      .getOrCreate('p1', 'player')
      .inMesh('play')
      .timeout(OPERATION_TIMEOUT_MS)
      .submit();
    assert.equal(result.status, 'created');
    assert.equal(String(result.actor.nodeRid), 'entry');
    assert.deepEqual(createdOn.entry, ['p1']);

    // No node has an Entry Spot type that accepts 'npc': no candidate, no factory call.
    await assert.rejects(() => actors.create('n1', 'npc').inMesh('play').timeout(1_000).submit());
    assert.deepEqual(createdOn.entry, ['p1']);
    assert.deepEqual(createdOn.plain, []);

    // An Actor that is already Ready is found without the Entry condition.
    const again = await actors
      .getOrCreate('p1', 'player')
      .inMesh('play')
      .timeout(OPERATION_TIMEOUT_MS)
      .submit();
    assert.equal(again.status, 'existing');
    assert.equal((await entry.get(nestjs.ZLINK_ACTOR_MANAGER).find('p1')).actorId, 'p1');
  } finally {
    for (const app of apps.reverse()) await app.close();
  }
});

test('a target without the Entry Spot type refuses before the factory and the requester excludes it', async () => {
  const provider = new ZLinkInMemoryProviderLocationStore();
  const createdOn = { stale: [], good: [], plain: [] };
  const apps = [];
  async function start(node, withEntry) {
    const builder = nestjs.zlinkFramework().addLocationStore(provider);
    builder.configureLocations().pollingIntervalMs(POLL_MS).routeCacheMaxAgeMs(0);
    const server = builder
      .addRouteMesh('play')
      .listen('tcp://127.0.0.1:0')
      .routingId(node)
      .objects()
      .server();
    if (withEntry) server.addEntrySpot(Entry, 'player');
    server.addActorFactory('player', newFactory(createdOn[node]), (f) => f.disableRelocation());
    const registration = builder.build();
    class TestModule {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(registration)] })(TestModule);
    const app = await NestFactory.createApplicationContext(TestModule, {
      logger: false,
      abortOnError: false
    });
    apps.push(app);
    return app;
  }
  try {
    const stale = await start('stale', true);
    const plain = await start('plain', false);
    const query = plain.get(nestjs.ZLINK_LOCATION_RUNTIME_QUERY);
    await until('stale descriptor accepts player', async () => {
      const items = (await query.listMeshNodeDescriptors('play')).items;
      return (
        items.find((d) => String(d.rid) === 'stale')?.objectCapabilities[0].entryAccepted ===
          true && plain.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('play').readyPeerCount >= 1
      );
    });
    // The descriptor is now stale: the node loses its Entry Spot type after publishing it.
    stale.get(nestjs.ZLINK_FRAMEWORK_REGISTRATION).spotNodes.get('play').entrySpotType = undefined;

    const actors = plain.get(nestjs.ZLINK_ACTOR_MANAGER);
    await assert.rejects(() =>
      actors.create('p1', 'player').inMesh('play').timeout(1_000).submit()
    );
    assert.deepEqual(createdOn.stale, [], 'the target refused before the factory');

    await start('good', true);
    await until('good descriptor published', async () => {
      const items = (await query.listMeshNodeDescriptors('play')).items;
      return (
        items.some((d) => String(d.rid) === 'good') &&
        plain.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('play').readyPeerCount >= 2
      );
    });
    for (let index = 0; index < 6; index += 1) {
      const result = await actors
        .create(`p-${index}`, 'player')
        .inMesh('play')
        .timeout(OPERATION_TIMEOUT_MS)
        .submit();
      assert.equal(result.status, 'created');
      assert.equal(String(result.actor.nodeRid), 'good');
    }
    assert.deepEqual(createdOn.stale, []);
    assert.equal(createdOn.good.length, 6);
  } finally {
    for (const app of apps.reverse()) await app.close();
  }
});

test('GetOrCreate returns a Ready Actor when no new-creation target remains', async () => {
  const actorId = 'p1';
  const authorityKey = encodeAuthorityKey('actor', actorId);
  const authority = {
    kind: 'snapshot',
    storeVersion: { value: 'v1' },
    payload: encodeActorAuthorityIdentity({
      actorType: 'player',
      actor: {
        actorId,
        objectGeneration: 3n,
        meshName: 'play',
        nodeRid: 'entry'
      },
      meshName: 'play',
      ownerNodeGeneration: 7n,
      owner: { ownerId: 'owner', leaseGeneration: 5n },
      spotId: 'entry',
      spotGeneration: 7n,
      spotKind: 'entry'
    }),
    objectGeneration: 3n,
    authorityOwnerGeneration: 11n,
    ownerId: 'owner',
    ownerLeaseGeneration: 5n,
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'player',
      descriptor: { meshName: 'play', rid: 'entry' },
      descriptorLifecycleGeneration: 7n,
      capacity: { actors: 1, spots: 0 }
    },
    storeNow: new Date(0)
  };
  let current = authority;
  let authorityReads = 0;
  const coordinator = new ZLinkActorPlacementCoordinator({
    store: {
      async readAuthority(key) {
        authorityReads += 1;
        assert.deepEqual(key, authorityKey);
        return current;
      },
      async reserve() {
        assert.fail('no placement target should reach reservation');
      }
    },
    target: async () => undefined,
    remoteCreate: async () => assert.fail('no placement target should reach remote creation')
  });

  await assert.rejects(
    () =>
      coordinator.create(actorId, 'player', true, 'play', Buffer.from('{}'), OPERATION_TIMEOUT_MS),
    (error) =>
      internalFrameworkErrorKind(error) ===
      ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted
  );
  assert.equal(authorityReads, 0);

  const result = await coordinator.create(
    actorId,
    'player',
    false,
    'play',
    Buffer.from('{}'),
    OPERATION_TIMEOUT_MS
  );
  assert.equal(result.status, 'existing');
  assert.equal(result.actor.actorId, actorId);
  assert.equal(String(result.actor.nodeRid), 'entry');
  assert.equal(result.actor.ownershipGeneration, 11n);
  assert.equal(result.actor.ownerLeaseGeneration, 5n);
  assert.equal(authorityReads, 1);

  current = { kind: 'missing', storeNow: new Date(0) };
  await assert.rejects(
    () =>
      coordinator.create(actorId, 'player', false, 'play', Buffer.from('{}'), OPERATION_TIMEOUT_MS),
    (error) =>
      internalFrameworkErrorKind(error) ===
      ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted
  );
  assert.equal(authorityReads, 2);
});

test('the requester excludes a target that refuses with routeNotConnected and picks another candidate', async () => {
  const nodes = ['node-a', 'node-b'].map((name) => RoutingId.from(name));
  const owner = { ownerId: 'owner', leaseGeneration: 1n };
  const targetsAsked = [];
  const remoteAsked = [];
  const store = {
    async reserve(request) {
      const target = request.target;
      return {
        kind: 'reserved',
        creating: {
          kind: 'snapshot',
          objectGeneration: 1n,
          authorityOwnerGeneration: 1n,
          ownerId: owner.ownerId,
          ownerLeaseGeneration: owner.leaseGeneration,
          storeVersion: { value: `v-${String(target.nodeRid)}` },
          payload: Buffer.alloc(0),
          allocation: { state: 'reserved' },
          pendingCreation: { reservationId: `r-${String(target.nodeRid)}` }
        }
      };
    },
    async readCreationTerminal() {
      return { kind: 'missing' };
    }
  };
  const coordinator = new ZLinkActorPlacementCoordinator({
    store,
    target: async (meshName, stableType, signal, excluded) => {
      targetsAsked.push([...excluded]);
      const index = excluded.size;
      if (index >= nodes.length) return undefined;
      return {
        meshName: 'play',
        nodeRid: nodes[index],
        nodeGeneration: 1n,
        entrySpotId: 'entry',
        owner,
        isLocal: false,
        sourceNodeRid: RoutingId.from('requester'),
        sourceNodeGeneration: 1n
      };
    },
    remoteCreate: async (meshName, targetNodeRid) => {
      remoteAsked.push(targetNodeRid);
      if (targetNodeRid === 'node-a')
        return internalFrameworkWireReply(ZLinkFrameworkInternalErrorKind.RouteNotConnected);
      return {
        terminalResult: RequestResult.Ok,
        failureCode: 0,
        tail: {
          kind: 'actorCreate',
          createResult: 'created',
          actor: { actorId: 'p1', generation: 1n, nodeRid: targetNodeRid }
        }
      };
    }
  });
  const result = await coordinator.create('p1', 'player', false, 'play', Buffer.from('{}'), 5_000);
  assert.equal(result.status, 'created');
  assert.equal(String(result.actor.nodeRid), 'node-b');
  assert.deepEqual(remoteAsked, ['node-a', 'node-b']);
  assert.equal(targetsAsked[1].length, 1);
});
