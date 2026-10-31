const assert = require('node:assert/strict');
const test = require('node:test');
process.env.NODE_ENV = 'test';
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkAutoConnectPlanner
} = require('../../packages/framework/dist/runtime/locations/auto-connect-planner');

for (const [rid, live, expected] of [
  ['remote', true, 1],
  ['remote', false, 0],
  ['local', true, 0]
]) {
  test(`same endpoint uses RID ${rid} and ${live ? 'live' : 'expired'} owner lease`, async () => {
    const now = new Date('2026-01-01T00:00:00Z');
    const tracker = new internal.ZLinkOwnerLeaseTracker({
      store: {
        async readOwnerLease() {
          return {
            kind: 'found',
            token: { ownerId: 'owner', leaseGeneration: 1n },
            storeNow: now,
            leaseExpiresAt: new Date(now.getTime() + (live ? 30000 : 0))
          };
        }
      },
      options: { pollingIntervalMs: 0 }
    });
    const local = {
      autoConnectType: internal.ZLinkLocationAutoConnectType.RouteMesh,
      meshName: 'mesh',
      role: internal.ZLinkLocationRole.Router,
      nodeRid: 'local',
      endpoint: 'tcp://shared:1'
    };
    const peer = { ...local, nodeRid: rid, generation: 1n, ownerId: 'owner', draining: false };
    const resolver = new internal.ZLinkStoreLocationResolvers({
      stores: {
        peerStore: {
          async listPeers() {
            return [peer];
          }
        }
      },
      leaseTracker: tracker,
      spotMeshNames: [],
      routeCacheMaxAgeMs: 0
    });
    const peers = await resolver.listLivePeers({
      autoConnectType: local.autoConnectType,
      meshName: local.meshName,
      role: local.role
    });
    assert.equal(peers.length, live ? 1 : 0);
    assert.equal(ZLinkAutoConnectPlanner.computeDesired(local, peers).size, expected);
    assert.equal(ZLinkAutoConnectPlanner.computeCandidates(local, peers).size, expected);
  });
}
