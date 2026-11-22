'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: boundary } = require('node:timers/promises');
const framework = require('../../packages/framework/dist/internal');
const { createMeshHost } = require('./fixtures/mesh-host');
const { encodeRoutingIdStorageHex } = require('../../packages/framework/dist/runtime/routing-id');
const {
  ZLinkRequestSourceResolver
} = require('../../packages/framework/dist/runtime/locations/request-source-resolver');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  internalFrameworkErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

function remoteDescriptor(kind) {
  return {
    meshName: 'play',
    rid: 'remote-node',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    state: 1,
    objectRole: 'server',
    placementWeight: 100,
    ownerId: 'remote-owner',
    leaseGeneration: 1n,
    entrySpotId: 'remote-entry',
    objectCapabilities: [{ objectKind: kind, stableType: 'room', entryAccepted: true }],
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 1 },
      spots: { active: 0, reserved: 0, limit: 1 },
      spotTypes: []
    }
  };
}

function resolver() {
  return new ZLinkRequestSourceResolver({
    async listLiveMeshNodes() {
      return [];
    },
    async remainingOwnerTokenLeaseMs() {
      return 30000;
    }
  });
}

test('a drained candidate and an empty Store produce Unavailable after one refresh', async () => {
  let reads = 0;
  const sources = new ZLinkRequestSourceResolver({
    async listLiveMeshNodes() {
      reads++;
      return [];
    },
    async remainingOwnerTokenLeaseMs() {
      return 30000;
    }
  });
  await sources.observeLiveDescriptors('play', [{ ...remoteDescriptor('user_spot'), state: 4 }]);
  assert.equal(await sources.selectPlacement('play', 'user_spot', 'room'), undefined);
  assert.equal(reads, 1);
});
for (let iteration = 1; iteration <= 40; iteration++) {
  for (const kind of ['actor', 'user_spot']) {
    test(`${iteration}: ${kind} reads a replacement once and publishes it to subsequent creates`, async () => {
      const replacement = remoteDescriptor(kind);
      let reads = 0;
      const sources = new ZLinkRequestSourceResolver({
        async listLiveMeshNodes(meshName) {
          assert.equal(meshName, 'play');
          reads++;
          return [replacement];
        },
        async remainingOwnerTokenLeaseMs() {
          return 30000;
        }
      });
      await sources.observeLiveDescriptors('play', [
        { ...replacement, rid: 'old-node', state: 4, placementWeight: 0 }
      ]);
      assert.equal(await sources.selectPlacement('play', kind, 'room'), replacement);
      assert.equal(reads, 1);
      assert.equal(await sources.selectPlacement('play', kind, 'room'), replacement);
      assert.equal(reads, 1);
    });
  }
}

for (let iteration = 1; iteration <= 40; iteration++) {
  test(`${iteration}: cold Instance Spot placement shares the replacement observation`, async () => {
    const replacement = remoteDescriptor('instance_spot');
    let reads = 0;
    const sources = new ZLinkRequestSourceResolver({
      async listLiveMeshNodes() {
        reads++;
        return [replacement];
      },
      async remainingOwnerTokenLeaseMs() {
        return 30000;
      }
    });
    await sources.observeLiveDescriptors('play', [{ ...replacement, rid: 'old-node', state: 4 }]);
    const node = { status: () => ({ routingId: replacement.rid, lifecycleGeneration: 1n }) };
    const signal = new AbortController().signal;
    for (const type of ['room', undefined]) {
      const result = await sources.selectInstancePlacement('play', type, node, signal);
      assert.deepEqual(result, { kind: 'selected', descriptor: replacement, stableType: 'room' });
      assert.equal(reads, 1);
    }
  });
}
test('placement excludes only the host generation that rejected the reservation', async () => {
  const sources = resolver();
  const remote = remoteDescriptor('user_spot');
  const excluded = new Map([[encodeRoutingIdStorageHex(remote.rid), remote.lifecycleGeneration]]);
  const select = () =>
    sources.selectPlacement('play', 'user_spot', 'room', undefined, undefined, excluded);
  await sources.observeLiveDescriptors('play', [remote]);
  assert.equal(await select(), undefined);

  const restarted = { ...remote, lifecycleGeneration: remote.lifecycleGeneration + 1n };
  await sources.observeLiveDescriptors('play', [restarted]);
  assert.equal(await select(), restarted);
});

for (let iteration = 1; iteration <= 40; iteration++) {
  for (const kind of ['actor', 'user_spot']) {
    for (const initial of ['before first poll', 'after clear']) {
      test(`${iteration}: ${kind} ${initial} selects the remote-only candidate on publication`, async () => {
        const sources = resolver();
        const remote = remoteDescriptor(kind);
        if (initial === 'after clear') {
          await sources.observeLiveDescriptors('play', [remote]);
          sources.clear();
        }
        const node = {
          status: () => ({ routingId: 'local-client', lifecycleGeneration: 1n }),
          isPeerRouteReady: (rid, generation) => rid === remote.rid && generation === 1n
        };
        let settled = false;
        const selected = sources.selectPlacement('play', kind, 'room', node).finally(() => {
          settled = true;
        });
        await boundary();
        const settledBeforePublication = settled;
        await sources.observeLiveDescriptors('play', [remote]);
        assert.equal(await selected, remote);
        assert.equal(settledBeforePublication, false);
      });
    }
    test(`${iteration}: ${kind} absent type returns undefined after one discovery publication`, async () => {
      const sources = resolver();
      const incompatible = { ...remoteDescriptor(kind), objectCapabilities: [] };
      let settled = false;
      const selected = sources.selectPlacement('play', kind, 'room').finally(() => {
        settled = true;
      });
      await boundary();
      const settledBeforePublication = settled;
      await sources.observeLiveDescriptors('play', [incompatible]);
      assert.equal(await selected, undefined);
      assert.equal(settledBeforePublication, false);
    });
  }
}

function emptyMeshCreate(t, kind) {
  const sources = resolver();
  const { host, meshRuntime } = createMeshHost({
    registration: framework.createFrameworkRegistration({
      locations: { storeInstance: new framework.ZLinkInMemoryProviderLocationStore() },
      spotNodes: {
        play: {
          router: { bind: 'tcp://local-play', routingId: 'local-client' },
          objectRole: 'client'
        }
      }
    })
  });
  meshRuntime.locationAutoConnect = { requestSources: sources };
  meshRuntime.meshNode = () => ({
    registerUserSpotOperationHandler() {},
    registerActorOperationHandler() {},
    status: () => ({ routingId: 'local-client', lifecycleGeneration: 1n })
  });
  const store = ZLinkLocationStoreRepository.prototype;
  t.mock.method(store, 'readAuthority', async () => ({ kind: 'missing' }));
  t.mock.method(store, 'readCreationTerminal', async () => ({ kind: 'missing' }));
  t.mock.method(store, 'reserve', async () => assert.fail('absent type must not reserve'));
  const spots = host.createPublicSpotManager({});
  const { placementCreate } = host.createActorManagerOptions();
  return {
    sources,
    create: (timeoutMs) =>
      kind === 'actor'
        ? placementCreate('actor', 'room', false, { meshName: 'play', timeoutMs })
        : spots.getOrCreate('spot', 'room').inMesh('play').timeout(timeoutMs).submit()
  };
}

for (let iteration = 1; iteration <= 40; iteration++) {
  for (const kind of ['actor', 'user_spot']) {
    for (const deadlineMs of [1000, 10]) {
      test(`${iteration}: ${kind} absent type, poll=50ms deadline=${deadlineMs}ms`, async (t) => {
        const f = emptyMeshCreate(t, kind);
        // No descriptor in this mesh advertises the requested stable type.
        const rows = [{ ...remoteDescriptor(kind), objectCapabilities: [] }];
        await f.sources.observeLiveDescriptors('play', rows);
        let publications = 0;
        const started = performance.now();
        const poll = setTimeout(() => {
          publications++;
          void f.sources.observeLiveDescriptors('play', rows);
        }, 50);
        try {
          const error = await f.create(deadlineMs).then(
            () => assert.fail('absent type must fail'),
            (error) => error
          );
          const elapsedMs = performance.now() - started;
          const fine = internalFrameworkErrorKind(error);
          t.diagnostic(
            JSON.stringify({
              kind,
              deadlineMs,
              pollMs: 50,
              elapsedMs,
              publications,
              errorKind: error.kind,
              internalKind: fine
            })
          );
          assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
          assert.equal(fine, 'placementCapacityExhausted');
          assert.equal(publications, 0, 'an observed empty snapshot must not await another poll');
        } finally {
          clearTimeout(poll);
        }
      });
    }
  }
}
