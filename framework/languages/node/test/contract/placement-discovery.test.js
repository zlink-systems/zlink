'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: boundary } = require('node:timers/promises');
const framework = require('../../packages/framework/dist/internal');
const { createMeshHost } = require('./fixtures/mesh-host');
const {
  createSpotNodeLocationAutoConnectContext
} = require('../../packages/framework/dist/runtime/spots/spot-node-autoconnect');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');

function descriptor(kind, state = 0) {
  return {
    meshName: 'play',
    rid: 'node-a',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    state,
    objectRole: 'server',
    placementWeight: 100,
    ownerId: 'owner',
    leaseGeneration: 1n,
    entrySpotId: 'entry-a',
    objectCapabilities: [{ objectKind: kind, stableType: 'room', entryAccepted: true }],
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 1 },
      spots: { active: 0, reserved: 0, limit: 1 },
      spotTypes: [{ objectKind: kind, stableType: 'room', active: 0, reserved: 0, limit: 1 }]
    }
  };
}

async function fixture(t, kind, initial) {
  let rows = initial;
  let live = true;
  let routeReady = false;
  let lists = 0;
  const reservations = [];
  const runtime = {
    async listLiveMeshNodes() {
      lists++;
      return rows;
    },
    async remainingOwnerTokenLeaseMs() {
      return live ? 30000 : 0;
    }
  };
  const discovery = createSpotNodeLocationAutoConnectContext(runtime, {}, {});
  await discovery.requestSources.observeLiveDescriptors('play', rows);
  const { host, meshRuntime } = createMeshHost({
    registration: framework.createFrameworkRegistration({
      locations: { storeInstance: new framework.ZLinkInMemoryProviderLocationStore() },
      spotNodes: {
        play: { router: { bind: 'tcp://local-play', routingId: 'node-a' }, objectRole: 'client' }
      }
    })
  });
  meshRuntime.locationAutoConnect = discovery;
  meshRuntime.meshNode = () => ({
    registerUserSpotOperationHandler() {},
    registerActorOperationHandler() {},
    isPeerRouteReady: () => routeReady,
    status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n })
  });
  const store = ZLinkLocationStoreRepository.prototype;
  t.mock.method(store, 'listMeshNodes', async () => {
    lists++;
    return { items: rows };
  });
  t.mock.method(store, 'readAuthority', async () => ({ kind: 'missing' }));
  t.mock.method(store, 'readCreationTerminal', async () => ({ kind: 'missing' }));
  t.mock.method(store, 'readOwnerLease', async (ownerId) => ({
    kind: 'found',
    token: { ownerId, leaseGeneration: 1n },
    leaseExpiresAt: new Date(live ? 31000 : 1000),
    storeNow: new Date(1000)
  }));
  t.mock.method(store, 'reserve', async (request) => {
    reservations.push(request);
    return { kind: 'typeMismatch' };
  });
  const spots = host.createPublicSpotManager({});
  const { placementCreate } = host.createActorManagerOptions();
  const create = () =>
    kind === 'actor'
      ? placementCreate('actor', 'room', false, { meshName: 'play', timeoutMs: 1000 })
      : spots.getOrCreate('spot', 'room').inMesh('play').timeout(1000).submit();
  return {
    reservations,
    makeRouteReady() {
      routeReady = true;
    },
    create,
    lists: () => lists,
    async refresh(next, leaseLive = true) {
      rows = next;
      live = leaseLive;
      await discovery.requestSources.observeLiveDescriptors('play', rows);
    }
  };
}

for (let iteration = 1; iteration <= 40; iteration++) {
  for (const kind of ['actor', 'user_spot']) {
    for (const change of ['serving', 'removed', 'weight', 'capacity', 'lease', 'deadline']) {
      test(`${iteration}: ${kind} Preparing discovery -> ${change}`, async (t) => {
        const row = descriptor(kind);
        const f = await fixture(t, kind, [row]);
        if (change === 'deadline') t.mock.timers.enable({ apis: ['setTimeout'] });
        let settled = false;
        const result = f
          .create()
          .then(
            (value) => ({ value }),
            (error) => ({ error })
          )
          .finally(() => {
            settled = true;
          });
        // Drain operation continuations; no elapsed-time sleep controls discovery.
        await boundary();
        assert.equal(settled, false, 'compatible Preparing must remain pending');
        assert.equal(f.reservations.length, 0);
        const reads = f.lists();
        assert.equal(reads, 0, 'a compatible Preparing candidate waits for publication');
        if (change === 'deadline') t.mock.timers.tick(1000);
        else {
          const next = { ...row, state: change === 'serving' ? 1 : 0 };
          if (change === 'weight') next.placementWeight = 0;
          if (change === 'capacity')
            next.populationCapacity = {
              actors: { active: 1, reserved: 0, limit: 1 },
              spots: { active: 1, reserved: 0, limit: 1 },
              spotTypes: []
            };
          await f.refresh(change === 'removed' ? [] : [next], change !== 'lease');
        }
        const { error } = await result;
        assert.ok(error);
        if (change === 'serving') {
          assert.match(error.message, /another stable type/);
          assert.equal(f.reservations.length, 1);
        } else {
          assert.equal(
            error.kind,
            change === 'deadline'
              ? framework.ZLinkFrameworkErrorKind.DeadlineExceeded
              : framework.ZLinkFrameworkErrorKind.Unavailable
          );
          assert.equal(f.reservations.length, 0);
        }
        const refreshes = change === 'serving' || change === 'deadline' ? 0 : 1;
        assert.equal(f.lists(), reads + refreshes);
      });
    }
    for (const input of ['empty', 'incompatible', 'weight', 'serving']) {
      test(`${iteration}: ${kind} immediate ${input} control`, async (t) => {
        const row = descriptor(kind, 1);
        if (input === 'incompatible') row.objectCapabilities = [];
        if (input === 'weight') row.placementWeight = 0;
        const f = await fixture(t, kind, input === 'empty' ? [] : [row]);
        const reads = f.lists();
        await assert.rejects(f.create(), (error) =>
          input === 'serving'
            ? /another stable type/.test(error.message)
            : error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
        );
        assert.equal(f.reservations.length, input === 'serving' ? 1 : 0);
        assert.equal(f.lists(), reads + (input === 'serving' ? 0 : 1));
      });
    }
  }
}

for (let iteration = 1; iteration <= 40; iteration++) {
  for (const kind of ['actor', 'user_spot']) {
    test(`${iteration}: ${kind} concurrent creates share discovery; publication crosses subscription`, async (t) => {
      const f = await fixture(t, kind, [descriptor(kind)]);
      const reads = f.lists();
      let settled = 0;
      const operations = Array.from({ length: 12 }, () =>
        f
          .create()
          .then(
            (value) => ({ value }),
            (error) => ({ error })
          )
          .finally(() => {
            settled++;
          })
      );
      await boundary();
      assert.equal(settled, 0);
      assert.equal(f.lists(), reads);
      assert.equal(f.reservations.length, 0);
      // Refresh publication and the next caller's subscription share the owner event loop.
      const refresh = f.refresh([descriptor(kind, 1)]);
      operations.push(
        f.create().then(
          (value) => ({ value }),
          (error) => ({ error })
        )
      );
      await refresh;
      for (const { error } of await Promise.all(operations))
        assert.match(error.message, /another stable type/);
      assert.equal(f.reservations.length, 13);
      assert.equal(f.lists(), reads);
    });
  }
}

for (let iteration = 1; iteration <= 40; iteration++) {
  for (const kind of ['actor', 'user_spot']) {
    for (const removed of [false, true]) {
      test(`${iteration}: ${kind} remote Serving route readiness observes ${removed ? 'removal' : 'current descriptor'}`, async (t) => {
        const row = { ...descriptor(kind, 1), rid: 'remote-node' };
        const f = await fixture(t, kind, [row]);
        let settled = false;
        const result = f
          .create()
          .then(
            (value) => ({ value }),
            (error) => ({ error })
          )
          .finally(() => {
            settled = true;
          });
        await boundary();
        assert.equal(settled, false);
        assert.equal(f.reservations.length, 0);
        f.makeRouteReady();
        await f.refresh(removed ? [] : [row]);
        const { error } = await result;
        if (removed) assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
        else assert.match(error.message, /another stable type/);
        assert.equal(f.reservations.length, removed ? 0 : 1);
      });
    }
  }
}
