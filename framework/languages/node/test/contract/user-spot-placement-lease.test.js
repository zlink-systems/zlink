'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { createMeshHost } = require('./fixtures/mesh-host');

for (const leaseState of ['expired', 'missing', 'replaced', 'live']) {
  test(`User Spot placement validates a ${leaseState} descriptor owner lease`, async () => {
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
    const store = host.locationOwner.locationStore();
    const signal = new AbortController().signal;
    const leaseReads = [];
    store.listMeshNodes = async () => ({ items: [descriptor] });
    store.readOwnerLease = async (ownerId, readSignal) => {
      leaseReads.push({ ownerId, signal: readSignal });
      return leaseState === 'missing'
        ? { kind: 'missing' }
        : {
            kind: 'found',
            token: { ownerId, leaseGeneration: leaseState === 'replaced' ? 2n : 1n },
            leaseExpiresAt: new Date(leaseState === 'expired' ? 999 : 31_000),
            storeNow: new Date(1000)
          };
    };
    meshRuntime.meshNode = () => ({
      status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n })
    });
    host.createPublicSpotManager({});

    const selected = await host.userSpotCoordinator.options.target(
      { meshName: 'play', stableType: 'room' },
      signal
    );

    if (leaseState === 'live') {
      assert.equal(selected.nodeRid, descriptor.rid);
      assert.deepEqual(selected.owner, { ownerId: 'owner', leaseGeneration: 1n });
    } else {
      assert.equal(selected, undefined);
    }
    assert.deepEqual(leaseReads, [{ ownerId: 'owner', signal }]);
  });
}
