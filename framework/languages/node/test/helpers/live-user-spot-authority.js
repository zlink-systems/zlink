const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  encodeServiceUserSpotAuthorityPayload
} = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');

// Membership fixtures need canonical Ready authority, independently of legacy rows.
async function liveUserSpotAuthority(store, meshName, spotId, objectGeneration) {
  const ownerId = `membership-owner:${spotId}`;
  const owner = await store.claimOwnerLease(ownerId, 30_000);
  assert.equal(owner.kind, 'claimed');
  const nodeRid = zlink.RoutingId.from(`membership-node:${spotId}`);
  const target = { meshName, nodeRid, nodeLifecycleGeneration: 1n, owner: owner.token };
  assert.equal(
    (
      await store.updateMeshNode(
        {
          meshName,
          rid: nodeRid,
          lifecycleGeneration: 1n,
          descriptorRevision: 1n,
          endpoint: 'tcp://127.0.0.1:1',
          objectRole: framework.ZLinkObjectRole.Server,
          placementWeight: 1,
          populationCapacity: {
            actors: { active: 0, reserved: 0, limit: 1 },
            spots: { active: 0, reserved: 0, limit: 1 },
            spotTypes: []
          },
          activationConcurrency: { active: 0, limit: 1 },
          channelWeights: {},
          applicationVersion: 1n,
          spotTypes: [],
          objectCapabilities: [],
          state: framework.ZLinkFrameworkRuntimeState.Serving,
          securityIdentity: 'test',
          ownerId,
          leaseGeneration: owner.token.leaseGeneration,
          updatedAt: new Date(0)
        },
        framework.ZLinkLocationWriteIntent.NewClaim
      )
    ).status,
    framework.ZLinkLocationWriteStatus.Stored
  );
  const key = { kind: 'user_spot', globalId: spotId };
  // Allocate preceding incarnations through the provider counter, without private state writes.
  for (let generation = 1n; generation <= objectGeneration; ++generation) {
    const reserved = await store.reserve({
      key,
      intent: {
        stableType: 'game',
        requestContentReference: `fixture:${spotId}:${generation}`,
        requestSha256: Buffer.alloc(32, 1),
        requestEncodedSize: 1n
      },
      target,
      capacity: { actors: 0, spots: 1 },
      creatingPayload: Buffer.from([1])
    });
    assert.equal(reserved.kind, 'reserved');
    assert.equal(reserved.creating.objectGeneration, generation);
    const fence = {
      key,
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target
    };
    if (generation < objectGeneration) {
      assert.equal((await store.abort(fence)).kind, 'aborted');
    } else {
      assert.equal(
        (
          await store.commit({
            ...fence,
            readyPayload: encodeServiceUserSpotAuthorityPayload({
              state: 'ready',
              stableType: 'game',
              spotId,
              ownerId,
              ownerLeaseGeneration: owner.token.leaseGeneration,
              ownerMeshName: meshName,
              ownerNodeRid: String(nodeRid),
              ownerNodeGeneration: 1n
            })
          })
        ).kind,
        'committed'
      );
    }
  }
}

module.exports = { liveUserSpotAuthority };
