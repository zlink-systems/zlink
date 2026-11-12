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

for (const leaseState of ['expired', 'missing', 'replaced', 'live']) {
  test(`Actor placement validates a ${leaseState} descriptor owner lease`, async (t) => {
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
      entrySpotId: 'entry-a',
      objectCapabilities: [{ objectKind: 'actor', stableType: 'room', entryAccepted: true }],
      populationCapacity: {
        spots: { active: 0, reserved: 0, limit: 1 },
        spotTypes: [
          { objectKind: 'actor', stableType: 'room', active: 0, reserved: 0, limit: 1 }
        ]
      }
    };
    const store = ZLinkLocationStoreRepository.prototype;
    const reservations = [];
    const transportCalls = [];
    const leaseReads = [];
    t.mock.method(store, 'listMeshNodes', async () => ({ items: [descriptor] }));
    t.mock.method(store, 'readAuthority', async () => ({ kind: 'missing' }));
    t.mock.method(store, 'readCreationTerminal', async () => ({ kind: 'missing' }));
    // The test stops at the transport boundary, so no remote factory runs.
    t.mock.method(store, 'reserve', async (request) => {
      reservations.push(request.target);
      return {
        kind: 'reserved',
        creating: {
          objectGeneration: 1n,
          authorityOwnerGeneration: 1n,
          storeVersion: { value: 'reservation-version' },
          pendingCreation: { reservationId: 'actor-reservation' }
        }
      };
    });
    t.mock.method(store, 'readOwnerLease', async (ownerId, readSignal) => {
      leaseReads.push({ ownerId, signal: readSignal });
      return leaseState === 'missing'
        ? { kind: 'missing' }
        : {
            kind: 'found',
            token: { ownerId, leaseGeneration: leaseState === 'replaced' ? 2n : 1n },
            leaseExpiresAt: new Date(leaseState === 'expired' ? 1000 : 31_000),
            storeNow: new Date(1000)
          };
    });
    meshRuntime.meshNode = () => ({
      registerUserSpotOperationHandler() {},
      registerActorOperationHandler() {},
      async requestActorCreate(...args) {
        transportCalls.push(args);
        throw new Error('Unexpected Actor transport call.');
      },
      status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n })
    });
    host.createPublicSpotManager({});
    const { placementCreate } = host.createActorManagerOptions();
    const create = () => placementCreate(
      'lease-actor', 'room', false, { meshName: 'play', timeoutMs: 1000 }
    );

    if (leaseState === 'live') {
      await assert.rejects(create(), /Unexpected Actor transport call\./u);
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
          error.message === "No eligible Actor placement target is ready for 'room'."
      );
      assert.deepEqual(reservations, []);
      assert.equal(leaseReads.length, 1);
      assert.equal(leaseReads[0].ownerId, 'owner');
      assert.ok(leaseReads[0].signal instanceof AbortSignal);
    }
    assert.equal(transportCalls.length, leaseState === 'live' ? 1 : 0);
    if (leaseState === 'live') {
      assert.equal(transportCalls[0][0], descriptor.rid);
      assert.equal(transportCalls[0][1].actorId, 'lease-actor');
    }
  });
}
