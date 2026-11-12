const assert = require('node:assert/strict');
const test = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const framework = require('../../packages/framework/dist/internal');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const {
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  ZLinkAuthoritySpotRouteResolver
} = require('../../packages/framework/dist/runtime/locations/resolvers');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');

for (const count of [0, 32, 2048]) {
  test(`idle_authority_reads_do_not_scale_after_startup_recovery/${count}`, async (t) => {
    const clock = fixedRidHostClock(t);
    const inner = new framework.ZLinkInMemoryProviderLocationStore();
    const calls = { reads: 0, authorityReads: 0, maintenanceReads: 0, scans: 0, rows: 0 };
    const provider = {
      read(key, signal) {
        calls.reads++;
        if (key.value.startsWith('authority')) calls.authorityReads++;
        if (
          key.value.startsWith('mesh-node') ||
          key.value.startsWith('owner') ||
          key.value.startsWith('zlink:v11:capacity:')
        )
          calls.maintenanceReads++;
        return inner.read(key, signal);
      },
      async scan(request, signal) {
        const result = await inner.scan(request, signal);
        if (request.prefix.startsWith('authority')) {
          calls.scans++;
          if (result.kind === 'page') calls.rows += result.value.items.length;
        }
        return result;
      },
      write: (request, signal) => inner.write(request, signal)
    };
    const repository = new framework.ZLinkLocationStoreRepository(
      provider,
      () => new Date(),
      undefined,
      () => performance.now(),
      1
    );
    const lease = await repository.claimOwnerLease('remote-owner', 30000);
    assert.equal(lease.kind, 'claimed');
    const descriptor = {
      meshName: 'remote',
      rid: 'remote-node',
      lifecycleGeneration: 1n,
      descriptorRevision: 1n,
      endpoint: 'tcp://127.0.0.1:1',
      objectRole: framework.ZLinkObjectRole.Server,
      placementWeight: 1,
      populationCapacity: {
        actors: { active: 0, reserved: 0, limit: 4096 },
        spots: { active: 0, reserved: 0, limit: 4096 },
        spotTypes: [
          { objectKind: 'instance_spot', stableType: 'room', active: 0, reserved: 0, limit: 4096 }
        ]
      },
      activationConcurrency: { active: 0, limit: 4096 },
      channelWeights: {},
      applicationVersion: 1n,
      spotTypes: ['room'],
      objectCapabilities: [
        {
          objectKind: 'actor',
          stableType: 'player',
          policy: 'disabled',
          hasSnapshotAdapter: false,
          limit: 4096
        },
        {
          objectKind: 'instance_spot',
          stableType: 'room',
          policy: 'disabled',
          hasSnapshotAdapter: false,
          limit: 4096
        }
      ],
      state: framework.ZLinkFrameworkRuntimeState.Serving,
      securityIdentity: 'test',
      ownerId: lease.token.ownerId,
      leaseGeneration: lease.token.leaseGeneration,
      updatedAt: new Date()
    };
    await assert.rejects(
      repository.updateMeshNode(
        descriptor,
        framework.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
      )
    );
    clock.advance(3);
    await repository.updateMeshNode(
      descriptor,
      framework.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
    );
    const target = {
      meshName: 'remote',
      nodeRid: 'remote-node',
      nodeLifecycleGeneration: 1n,
      owner: lease.token
    };
    for (let index = 0; index < count; index++) {
      const actor = index % 2 === 0;
      const id = `idle-${index}`;
      const kind = actor ? 'actor' : 'instance_spot';
      const stableType = actor ? 'player' : 'room';
      const key = { kind, globalId: id };
      const payload = actor
        ? encodeActorAuthorityIdentity({
            actorType: stableType,
            actor: { actorId: id, nodeRid: 'remote-node', generation: 1n },
            meshName: 'remote',
            ownerNodeGeneration: 1n,
            owner: lease.token,
            spotId: 'remote-entry',
            spotGeneration: 1n,
            spotKind: framework.ZLinkSpotKind.Entry
          })
        : framework.encodeServiceInstanceAuthorityPayload({
            state: 'ready',
            stableType,
            spotId: id,
            ownerId: lease.token.ownerId,
            ownerLeaseGeneration: lease.token.leaseGeneration,
            ownerMeshName: 'remote',
            ownerNodeRid: 'remote-node',
            ownerNodeGeneration: 1n
          });
      const reserved = await repository.reserve({
        key,
        actorRelocationPolicy: 'disabled',
        intent: {
          stableType,
          requestContentReference: `request-${index}`,
          requestSha256: Buffer.alloc(32, 1),
          requestEncodedSize: 1n
        },
        target,
        creatingPayload: Buffer.from('creating'),
        capacity: actor
          ? { actors: 1, spots: 0 }
          : {
              actors: 0,
              spots: 1,
              spotType: { objectKind: kind, stableType, count: 1 }
            }
      });
      assert.equal(reserved.kind, 'reserved');
      const committed = await repository.commit({
        key,
        target,
        reservationId: reserved.reservationId,
        expectedStoreVersion: reserved.creating.storeVersion.value,
        readyPayload: payload
      });
      assert.equal(committed.kind, 'committed');
    }
    class Room {
      constructor(context) {
        this.context = context;
      }
    }
    const builder = nestjs.zlinkFramework().addLocationStore(provider);
    builder.configureLocations().ownerLeaseFencingMarginMs(1).pollingIntervalMs(10);
    builder
      .addRouteMesh('play')
      .listen('tcp://127.0.0.1:0')
      .routingId(`idle-host-${count}`)
      .objects()
      .server()
      .addInstanceSpotFactory('room', Room, (factory) => factory.disableRelocation());
    class AppModule {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())] })(AppModule);
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    try {
      const host = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
      await host.start();
      await finishFixedRidHostStartup(host, clock, framework.ZLinkFrameworkRuntimeState.Preparing);
      assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
      assert.ok(calls.scans > 0, 'startup recovery must scan authority');
      assert.ok(calls.rows >= count, 'startup recovery must read every seeded authority');
      const baseline = { ...calls };
      await delay(50);
      const delta = Object.fromEntries(
        Object.keys(calls).map((key) => [key, calls[key] - baseline[key]])
      );
      t.diagnostic(JSON.stringify({ count, startup: baseline, idle: delta }));
      assert.equal(delta.scans, 0);
      assert.equal(delta.rows, 0);
      assert.equal(delta.authorityReads, 0);
      assert.equal(delta.reads, delta.maintenanceReads);
      if (count > 0) {
        const resolver = new ZLinkAuthoritySpotRouteResolver(
          repository,
          (meshName) => meshName,
          undefined,
          host.locationOwner.currentLeaseTracker,
          15000
        );
        const beforeLookup = calls.authorityReads;
        const first = await resolver.resolve('idle-1');
        assert.equal(first.targetNodeRid, 'remote-node');
        assert.equal(calls.authorityReads, beforeLookup + 1);
        const second = await resolver.resolve('idle-1');
        assert.deepEqual(second, first);
        assert.equal(
          calls.authorityReads,
          beforeLookup + 1,
          'a warm Ready route must reuse its lookup'
        );
      }
    } finally {
      await app.close();
    }
  });
}

test('remote relocation reaches the new owner through cold lookup and warm cache expiry', async () => {
  let reads = 0;
  let now = 0;
  const snapshot = (node, generation, version) => ({
    kind: 'snapshot',
    storeVersion: { value: version },
    objectGeneration: 1n,
    authorityOwnerGeneration: generation,
    ownerId: `owner-${node}`,
    ownerLeaseGeneration: 1n,
    allocation: {
      state: 'active',
      objectKind: 'instance_spot',
      stableType: 'room',
      descriptor: { meshName: 'play', rid: node },
      descriptorLifecycleGeneration: 1n
    },
    payload: framework.encodeServiceInstanceAuthorityPayload({
      state: 'ready',
      stableType: 'room',
      spotId: 'remote-room',
      ownerId: `owner-${node}`,
      ownerLeaseGeneration: 1n,
      ownerMeshName: 'play',
      ownerNodeRid: node,
      ownerNodeGeneration: 1n
    }),
    storeNow: new Date()
  });
  let current = snapshot('source', 1n, 'source-v1');
  const store = {
    async readAuthority(key) {
      assert.equal(key.value, 'zla1:s:11:remote-room');
      reads++;
      return current;
    }
  };
  const lease = { remainingOwnerTokenLeaseMs: async () => 30000 };
  const warm = new ZLinkAuthoritySpotRouteResolver(
    store,
    (mesh) => mesh,
    undefined,
    lease,
    15000,
    () => now
  );
  assert.equal((await warm.resolve('remote-room')).targetNodeRid, 'source');
  assert.equal(reads, 1);
  current = snapshot('target', 7n, 'target-v2');
  assert.equal((await warm.resolve('remote-room')).targetNodeRid, 'source');
  assert.equal(reads, 1, 'a remote commit alone does not invalidate a valid Ready cache');
  const cold = new ZLinkAuthoritySpotRouteResolver(
    store,
    (mesh) => mesh,
    undefined,
    lease,
    15000,
    () => now
  );
  assert.equal((await cold.resolve('remote-room')).targetNodeRid, 'target');
  assert.equal(reads, 2);
  now = 15000;
  const refreshed = await warm.resolve('remote-room');
  assert.equal(refreshed.targetNodeRid, 'target');
  assert.equal(refreshed.authorityOwnerGeneration, 7n);
  assert.equal(refreshed.authorityStoreVersion, 'target-v2');
  assert.equal(reads, 3);
});
