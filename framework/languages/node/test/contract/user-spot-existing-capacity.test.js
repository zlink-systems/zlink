'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind, ZLinkSpotCreateState } = require('../../packages/framework/dist');

async function capacityOne(run) {
  let factoryCalls = 0;
  class Room {
    constructor() {
      factoryCalls++;
    }
  }
  Injectable({ scope: Scope.TRANSIENT })(Room);
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder
    .addRouteMesh('existing-capacity')
    .listen('tcp://127.0.0.1:0')
    .routingId('existing-capacity-node')
    .setSpotLimit(1)
    .objects()
    .server()
    .addSpotFactory('room', Room, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())], providers: [Room] })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  try {
    const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
    const original = await spots.getOrCreate('room-1', 'room').inMesh('existing-capacity').submit();
    assert.equal(original.state, ZLinkSpotCreateState.Created);
    await run(spots, original);
    assert.equal(factoryCalls, 1);
  } finally {
    await app.close();
  }
}

test('GetOrCreate returns the same Ready SpotRef when its target capacity is full', async () => {
  await capacityOne(async (spots, original) => {
    const existing = await spots.getOrCreate('room-1', 'room').inMesh('existing-capacity').submit();
    assert.equal(existing.state, ZLinkSpotCreateState.Existing);
    assert.deepEqual(existing.spot, original.spot);
  });
});

test('GetOrCreate still rejects another Spot ID when its target capacity is full', async () => {
  await capacityOne(async (spots) => {
    await assert.rejects(
      spots.getOrCreate('room-2', 'room').inMesh('existing-capacity').submit(),
      (error) =>
        error.kind === ZLinkFrameworkErrorKind.Unavailable &&
        error.message.includes('No eligible User Spot placement target is ready.')
    );
  });
});

const internal = require('../../packages/framework/dist/internal');
const { encodeRoutingIdStorageHex } = require('../../packages/framework/dist/runtime/routing-id');
const {
  ZLinkOwnerLeaseTracker
} = require('../../packages/framework/dist/runtime/locations/lease-tracker');
const {
  ZLinkUserSpotCreationCoordinator
} = require('../../packages/framework/dist/runtime/host/user-spot-creation-coordinator');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  ZLinkLocationWriteIntent
} = require('../../packages/framework/dist/contracts/Locations/Writes');
function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
async function creationFixture(policy = 'disabled') {
  let now = new Date(1000);
  const entered = deferred(),
    released = deferred();
  const authorityEntered = deferred(),
    authorityReleased = deferred();
  let holdAuthority = false;
  let holdDescriptor = false,
    conflictRelease = false,
    releaseConflicts = 0;
  class Provider extends internal.ZLinkInMemoryProviderLocationStore {
    async read(key, signal) {
      if (holdDescriptor && key.value.startsWith('mesh-node\0')) {
        holdDescriptor = false;
        entered.resolve();
        await released.promise;
      }
      const result = await super.read(key, signal);
      if (holdAuthority && key.value.startsWith('authority\0') && result.kind === 'missing') {
        holdAuthority = false;
        authorityEntered.resolve();
        await authorityReleased.promise;
      }
      return result;
    }
    async write(request, signal) {
      if (conflictRelease && request.mutations.some((item) => item.kind === 'delete')) {
        conflictRelease = false;
        const mutation = request.mutations.find(
          (item) => item.kind === 'put' && item.key.value.startsWith('zlink:v11:capacity:')
        );
        assert.ok(mutation);
        const current = await super.read(mutation.key, signal);
        assert.equal(current.kind, 'found');
        const changed = await super.write(
          {
            conditions: [{ kind: 'version', key: mutation.key, expected: current.value.version }],
            mutations: [{ kind: 'put', key: mutation.key, bytes: current.value.bytes }]
          },
          signal
        );
        assert.equal(changed.kind, 'applied');
        const conflict = await super.write(request, signal);
        assert.equal(conflict.kind, 'conflict');
        releaseConflicts++;
        return conflict;
      }
      return super.write(request, signal);
    }
  }
  const provider = new Provider(() => now),
    outcomes = [];
  const pendingSeen = deferred();
  let watchPending = false;
  class Repository extends ZLinkLocationStoreRepository {
    async readAuthority(...args) {
      const current = await super.readAuthority(...args);
      if (watchPending && current.kind === 'snapshot' && current.allocation.state === 'reserved') {
        watchPending = false;
        pendingSeen.resolve();
      }
      return current;
    }
    async reserve(...args) {
      const result = await super.reserve(...args);
      outcomes.push(result.kind);
      return result;
    }
  }
  const store = new Repository(provider, () => now);
  const lease = await store.claimOwnerLease('owner', 30000);
  assert.equal(lease.kind, 'claimed');
  const owner = lease.token;
  const descriptor = {
    meshName: 'mesh',
    rid: 'node',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint: 'tcp://node',
    objectRole: 'server',
    placementWeight: 1,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 1 },
      spots: { active: 0, reserved: 0, limit: 1 },
      spotTypes: [{ objectKind: 'user_spot', stableType: 'room', active: 0, reserved: 0, limit: 1 }]
    },
    activationConcurrency: { active: 0, limit: 1 },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: ['room'],
    objectCapabilities: [
      { objectKind: 'user_spot', stableType: 'room', policy, hasSnapshotAdapter: false, limit: 1 }
    ],
    state: 1,
    securityIdentity: 'test',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: now
  };
  assert.equal(
    (await store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NewClaim)).status,
    internal.ZLinkLocationWriteStatus.Stored
  );
  let factoryCalls = 0;
  const coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    leaseTracker: new ZLinkOwnerLeaseTracker({ store, monotonicNowMs: () => now.getTime() }),
    relocationPolicy: () => policy,
    target: async (_request, signal, excluded) => {
      const live = await store.readOwnerLease(owner.ownerId, signal);
      if (
        live.kind !== 'found' ||
        live.leaseExpiresAt.getTime() <= live.storeNow.getTime() ||
        excluded?.has(encodeRoutingIdStorageHex('node'))
      )
        return undefined;
      const capacity = (await store.listMeshNodes('mesh', {}, signal)).items[0].populationCapacity
        .spots;
      if (capacity.active + capacity.reserved >= capacity.limit) return undefined;
      return { meshName: 'mesh', nodeRid: 'node', nodeGeneration: 1n, owner, isLocal: true };
    }
  });
  return {
    store,
    outcomes,
    observer: new ZLinkLocationStoreRepository(provider, () => now),
    get factoryCalls() {
      return factoryCalls;
    },
    get releaseConflicts() {
      return releaseConflicts;
    },
    expire() {
      now = new Date(31001);
    },
    conflictReleaseOnce() {
      conflictRelease = true;
    },
    observePending() {
      watchPending = true;
      return pendingSeen.promise;
    },
    blockMissingAuthority() {
      holdAuthority = true;
      return authorityEntered.promise;
    },
    releaseAuthority() {
      authorityReleased.resolve();
    },
    blockReserveDescriptor() {
      holdDescriptor = true;
      return entered.promise;
    },
    releaseDescriptor() {
      released.resolve();
    },
    async create(id, materialized) {
      return coordinator.getOrCreate(
        {
          meshName: 'mesh',
          spotId: id,
          stableType: 'room',
          requestPayload: Buffer.alloc(0),
          timeoutMs: 1000
        },
        async () => {
          factoryCalls++;
          const state = await materialized?.();
          return { spotId: id, state: state ?? ZLinkSpotCreateState.Created };
        }
      );
    }
  };
}
for (const conflict of [false, true]) {
  test(`expired Ready owner is released before capacity selection (conflict=${conflict})`, async () => {
    const f = await creationFixture();
    assert.equal((await f.create('expired-room')).result.state, ZLinkSpotCreateState.Created);
    f.expire();
    if (conflict) f.conflictReleaseOnce();
    await assert.rejects(
      f.create('expired-room'),
      (e) => e.kind === ZLinkFrameworkErrorKind.Unavailable
    );
    assert.equal(
      (await f.store.readAuthority(encodeAuthorityKey('user_spot', 'expired-room'))).kind,
      'missing'
    );
    assert.equal(
      (await f.observer.listMeshNodes('mesh')).items[0].populationCapacity.spots.active,
      0
    );
    assert.equal(f.factoryCalls, 1);
    assert.equal(f.releaseConflicts, conflict ? 1 : 0);
  });
}
test('expired Ready owner with relocation policy remains unavailable without unsafe release', async () => {
  const f = await creationFixture('recreate'),
    original = await f.create('retained-room');
  f.expire();
  await assert.rejects(
    f.create('retained-room'),
    (e) => e.kind === ZLinkFrameworkErrorKind.Unavailable
  );
  const current = await f.store.readAuthority(encodeAuthorityKey('user_spot', 'retained-room'));
  assert.equal(current.objectGeneration, original.spot.objectGeneration);
  assert.equal(current.allocation.state, 'active');
  assert.equal(f.factoryCalls, 1);
});
test('capacity race follows the same ID winner after a capacity-exhausted reservation', async () => {
  const f = await creationFixture(),
    entered = f.blockReserveDescriptor();
  const observed = f.create('racing-room').then(
    (value) => ({ value }),
    (error) => ({ error })
  );
  try {
    await entered;
    const winner = await f.create('racing-room');
    assert.equal(winner.result.state, ZLinkSpotCreateState.Created);
    f.releaseDescriptor();
    const result = await observed;
    if (result.error) throw result.error;
    assert.equal(result.value.result.state, ZLinkSpotCreateState.Existing);
    assert.deepEqual(result.value.spot, winner.spot);
    assert.ok(f.outcomes.includes('placementCapacityExhausted'));
    assert.equal(f.factoryCalls, 1);
  } finally {
    f.releaseDescriptor();
    await observed;
  }
});
test('provider descriptors project reserved, active and released capacity across repositories', async () => {
  const f = await creationFixture(),
    entered = deferred(),
    release = deferred();
  const creating = f.create('view-room', async () => {
    entered.resolve();
    await release.promise;
  });
  try {
    await entered.promise;
    const pending = (await f.observer.listMeshNodes('mesh')).items[0];
    assert.deepEqual(pending.populationCapacity.spots, { active: 0, reserved: 1, limit: 1 });
    release.resolve();
    const created = await creating;
    const active = (await f.observer.listMeshNodes('mesh')).items[0];
    assert.deepEqual(active.populationCapacity.spots, { active: 1, reserved: 0, limit: 1 });
    assert.equal(active.populationCapacity.spotTypes[0].active, 1);
    f.expire();
    const key = encodeAuthorityKey('user_spot', 'view-room'),
      authority = await f.store.readAuthority(key);
    assert.equal(
      await f.store.releaseEndedReservation(key, authority.storeVersion.value, undefined, {
        key: { kind: 'user_spot', globalId: 'view-room' },
        intent: { stableType: 'room' },
        actorRelocationPolicy: 'disabled'
      }),
      true
    );
    assert.ok(created.spot.objectGeneration > 0n);
    const final = (await f.observer.listMeshNodes('mesh')).items[0];
    assert.deepEqual(final.populationCapacity.spots, { active: 0, reserved: 0, limit: 1 });
    assert.equal(final.populationCapacity.spotTypes[0].active, 0);
    assert.equal(final.descriptorRevision, pending.descriptorRevision);
  } finally {
    release.resolve();
    await creating;
  }
});

test('capacity race follows a winner created after Missing and before candidate selection', async () => {
  const f = await creationFixture(),
    entered = f.blockMissingAuthority();
  const observed = f.create('selection-race').then(
    (value) => ({ value }),
    (error) => ({ error })
  );
  try {
    await entered;
    const winner = await f.create('selection-race');
    f.releaseAuthority();
    const result = await observed;
    if (result.error) throw result.error;
    assert.equal(result.value.result.state, ZLinkSpotCreateState.Existing);
    assert.deepEqual(result.value.spot, winner.spot);
    assert.equal(f.factoryCalls, 1);
  } finally {
    f.releaseAuthority();
    await observed;
  }
});

test('expired Ready authority protected by an aggregate never bypasses lease validation', async () => {
  const f = await creationFixture();
  const original = await f.create('aggregate-room');
  const key = encodeAuthorityKey('user_spot', 'aggregate-room');
  const snapshot = await f.store.readAuthority(key);
  const prepared = await f.store.prepareAggregate({
    aggregateId: { value: '44444444-4444-4444-8444-444444444444' },
    aggregateGeneration: 1n,
    participants: [
      {
        authorityKey: key,
        expectedStoreVersion: snapshot.storeVersion,
        ownerTransition: 'preserve',
        authorityPayload: snapshot.payload,
        membershipMutation: Buffer.alloc(0)
      }
    ],
    inventoryDigest: Buffer.alloc(32, 9),
    targetDescriptor: snapshot.allocation.descriptor,
    targetDescriptorLifecycleGeneration: snapshot.allocation.descriptorLifecycleGeneration,
    targetOwner: { ownerId: snapshot.ownerId, leaseGeneration: snapshot.ownerLeaseGeneration },
    capacity: { actors: 0, spots: 0 }
  });
  assert.equal(prepared.kind, 'prepared');
  assert.equal((await f.store.readAuthority(key)).storeVersion.value, snapshot.storeVersion.value);
  f.expire();
  await assert.rejects(
    f.create('aggregate-room'),
    (error) => error.kind === ZLinkFrameworkErrorKind.Unavailable
  );
  const retained = await f.store.readAuthority(key);
  assert.equal(retained.objectGeneration, original.spot.objectGeneration);
  assert.equal(f.factoryCalls, 1);
});

test('capacity race forgets stale capacity exclusion after another Creating attempt is rejected', async () => {
  const f = await creationFixture();
  const descriptorEntered = f.blockReserveDescriptor();
  const source = f.create('retry-room').then(
    (value) => ({ value }),
    (error) => ({ error })
  );
  const factoryEntered = deferred(),
    reject = deferred();
  let winner;
  try {
    await descriptorEntered;
    winner = f.create('retry-room', async () => {
      factoryEntered.resolve();
      await reject.promise;
      return ZLinkSpotCreateState.Rejected;
    });
    await factoryEntered.promise;
    const observed = f.observePending();
    f.releaseDescriptor();
    await observed;
    assert.ok(f.outcomes.includes('placementCapacityExhausted'));
    reject.resolve();
    const rejected = await winner;
    assert.equal(rejected.result.state, ZLinkSpotCreateState.Rejected);
    const result = await source;
    if (result.error) throw result.error;
    assert.equal(result.value.result.state, ZLinkSpotCreateState.Created);
    assert.ok(result.value.spot.objectGeneration > rejected.spot.objectGeneration);
    assert.equal(f.factoryCalls, 2);
  } finally {
    f.releaseDescriptor();
    reject.resolve();
    if (winner) await winner;
    await source;
  }
});
