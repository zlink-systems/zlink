'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkRequestSourceResolver
} = require('../../packages/framework/dist/runtime/locations/request-source-resolver');
const { encodeRoutingIdStorageHex } = require('../../packages/framework/dist/runtime/routing-id');

for (let iteration = 1; iteration <= 40; iteration++) {
  for (const update of ['removed', 'cleared']) {
    test(`${iteration}: late source read cannot republish placement after discovery ${update}`, async () => {
      let release;
      let entered;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const read = new Promise((resolve) => {
        release = resolve;
      });
      const row = {
        rid: 'node',
        meshName: 'play',
        lifecycleGeneration: 1n,
        ownerId: 'owner',
        leaseGeneration: 1n,
        state: 1,
        objectRole: 'server',
        placementWeight: 100,
        entrySpotId: 'entry',
        objectCapabilities: [{ objectKind: 'actor', stableType: 'room', entryAccepted: true }],
        populationCapacity: { actors: { active: 0, reserved: 0, limit: 1 } }
      };
      const resolver = new ZLinkRequestSourceResolver({
        async listLiveMeshNodes() {
          entered();
          return await read;
        },
        async remainingOwnerTokenLeaseMs() {
          return 30000;
        }
      });
      await resolver.observeLiveDescriptors('play', []);
      const pointRead = resolver.accept('play', 'node', 1n, encodeRoutingIdStorageHex('node'));
      await started;
      if (update === 'cleared') resolver.clear();
      else await resolver.observeLiveDescriptors('play', []);
      release([row]);
      await pointRead;
      assert.equal(await resolver.selectPlacement('play', 'actor', 'room'), undefined);
    });
  }
  test(`${iteration}: discovery shutdown and caller abort release placement subscriptions`, async () => {
    const resolver = new ZLinkRequestSourceResolver({
      async listLiveMeshNodes() {
        throw new Error('unexpected request scan');
      },
      async remainingOwnerTokenLeaseMs() {
        return 30000;
      }
    });
    const row = {
      rid: 'node',
      meshName: 'play',
      lifecycleGeneration: 1n,
      ownerId: 'owner',
      leaseGeneration: 1n,
      state: 0,
      objectRole: 'server',
      placementWeight: 100,
      entrySpotId: 'entry',
      objectCapabilities: [{ objectKind: 'actor', stableType: 'room', entryAccepted: true }],
      populationCapacity: { actors: { active: 0, reserved: 0, limit: 1 } }
    };
    await resolver.observeLiveDescriptors('play', [row]);
    const controller = new AbortController();
    const reason = new Error('caller abort reason');
    const aborted = resolver.selectPlacement('play', 'actor', 'room', undefined, controller.signal);
    controller.abort(reason);
    await assert.rejects(aborted, (error) => error === reason);
    const stopped = resolver.selectPlacement('play', 'actor', 'room');
    resolver.clear();
    assert.equal(await stopped, undefined);
  });
}

const {
  ZLinkActorPlacementCoordinator
} = require('../../packages/framework/dist/runtime/host/actor-placement-coordinator');
const {
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
for (let iteration = 1; iteration <= 40; iteration++) {
  for (const live of [false, true]) {
    test(`${iteration}: Ready Actor lookup requires a ${live ? 'live' : 'ended'} owner lease`, async () => {
      const owner = { ownerId: 'ready-owner', leaseGeneration: 1n };
      let reserves = 0;
      let targets = 0;
      const snapshot = {
        kind: 'snapshot',
        objectGeneration: 1n,
        authorityOwnerGeneration: 1n,
        ownerId: owner.ownerId,
        ownerLeaseGeneration: 1n,
        payload: encodeActorAuthorityIdentity({
          actorType: 'room',
          actor: {
            actorId: 'actor',
            objectGeneration: 1n,
            meshName: 'play',
            nodeRid: 'ready-node'
          },
          meshName: 'play',
          ownerNodeGeneration: 1n,
          owner,
          spotId: 'entry',
          spotGeneration: 1n,
          spotKind: 'entry'
        }),
        allocation: { state: 'active', objectKind: 'actor', stableType: 'room' }
      };
      const coordinator = new ZLinkActorPlacementCoordinator({
        leaseTracker: {
          async isOwnerTokenLive(token) {
            assert.deepEqual(token, owner);
            return live;
          }
        },
        store: {
          async readAuthority() {
            return snapshot;
          },
          async reserve() {
            reserves++;
            return { kind: 'typeMismatch' };
          }
        },
        async target() {
          targets++;
          return {
            meshName: 'play',
            nodeRid: 'live-node',
            nodeGeneration: 1n,
            sourceNodeRid: 'client',
            sourceNodeGeneration: 1n,
            entrySpotId: 'entry',
            owner: { ownerId: 'live-owner', leaseGeneration: 1n }
          };
        }
      });
      const create = () =>
        coordinator.create('actor', 'room', false, 'play', Buffer.from('{}'), 1000);
      if (live) assert.equal((await create()).status, 'existing');
      else await assert.rejects(create, /another stable type/);
      assert.equal(targets, live ? 0 : 1);
      assert.equal(reserves, live ? 0 : 1);
    });
  }
}
