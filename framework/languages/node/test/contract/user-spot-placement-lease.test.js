'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { createMeshHost } = require('./fixtures/mesh-host');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');

for (const leaseState of ['expired', 'missing', 'replaced', 'live']) {
  test(`User Spot placement validates a ${leaseState} descriptor owner lease`, async (t) => {
    const provider = new framework.ZLinkInMemoryProviderLocationStore();
    const { host, meshRuntime } = createMeshHost({
      registration: framework.createFrameworkRegistration({
        locations: { storeInstance: provider },
        spotNodes: {
          play: {
            router: { bind: 'tcp://local-play', routingId: 'node-a' },
            objectRole: 'client'
          }
        }
      })
    });
    const descriptor = {
      meshName: 'play',
      rid: 'node-a',
      lifecycleGeneration: 1n,
      state: framework.ZLinkFrameworkRuntimeState.Serving,
      objectRole: 'server',
      placementWeight: 100,
      ownerId: 'owner',
      leaseGeneration: 1n,
      objectCapabilities: [{ objectKind: 'user_spot', stableType: 'room' }],
      populationCapacity: {
        spots: { active: 0, reserved: 0, limit: 1 },
        spotTypes: [
          { objectKind: 'user_spot', stableType: 'room', active: 0, reserved: 0, limit: 1 }
        ]
      }
    };
    const store = ZLinkLocationStoreRepository.prototype;
    const reservations = [];
    const leaseReads = [];
    t.mock.method(store, 'listMeshNodes', async () => ({ items: [descriptor] }));
    t.mock.method(store, 'readAuthority', async () => ({ kind: 'missing' }));
    // Stop at the public Store reservation boundary: this test checks placement,
    // before factory execution and authority publication begin.
    t.mock.method(store, 'reserve', async (request) => {
      reservations.push(request.target);
      return { kind: 'typeMismatch' };
    });
    t.mock.method(store, 'readOwnerLease', async (ownerId, readSignal) => {
      leaseReads.push({ ownerId, signal: readSignal });
      return leaseState === 'missing'
        ? { kind: 'missing' }
        : {
            kind: 'found',
            token: { ownerId, leaseGeneration: leaseState === 'replaced' ? 2n : 1n },
            leaseExpiresAt: new Date(leaseState === 'expired' ? 999 : 31_000),
            storeNow: new Date(1000)
          };
    });
    meshRuntime.meshNode = () => ({
      registerUserSpotOperationHandler() {},
      status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n })
    });
    const spots = host.createPublicSpotManager({});
    const create = () => spots.getOrCreate('lease-room', 'room').inMesh('play').submit();

    if (leaseState === 'live') {
      await assert.rejects(create(), /User Spot 'lease-room' has another stable type\./u);
      assert.deepEqual(reservations, [
        {
          meshName: 'play',
          nodeRid: descriptor.rid,
          nodeLifecycleGeneration: 1n,
          owner: { ownerId: 'owner', leaseGeneration: 1n }
        }
      ]);
    } else {
      await assert.rejects(
        create(),
        (error) =>
          error.kind === framework.ZLinkFrameworkErrorKind.Unavailable &&
          error.message === 'No eligible User Spot placement target is ready.'
      );
      assert.deepEqual(reservations, []);
      assert.equal(leaseReads.length, 1);
      assert.equal(leaseReads[0].ownerId, 'owner');
      assert.ok(leaseReads[0].signal instanceof AbortSignal);
    }
  });
}
