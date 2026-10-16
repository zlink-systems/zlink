const assert = require('node:assert/strict');
const test = require('node:test');

const { MeshRouterResolver } = require('../../packages/framework/dist/runtime/host/mesh-router-resolver');

// Spec 08 E2: a node-direct send classifies its target against the prepared
// manual connections; the classification is the same for every routing-id form.
test('manual node target classification keeps its decision table', () => {
  const registration = (router) => ({
    spotNodes: new Map([['mesh', { router }]]),
    routeChannels: new Map()
  });
  const classify = (router, target) =>
    new MeshRouterResolver(registration(router)).classifyManualNodeTarget('mesh', target);
  assert.equal(new MeshRouterResolver(registration(undefined)).classifyManualNodeTarget('other', 'a'), undefined);
  assert.equal(classify({}, 'peer-a'), undefined);
  assert.equal(classify({ manualConnections: [], manualPeerConnections: [] }, 'peer-a'), undefined);
  const peers = { manualPeerConnections: [{ peerRid: 'peer-a', endpoint: 'tcp://127.0.0.1:1' }] };
  assert.equal(classify(peers, 'peer-a'), true);
  assert.equal(classify(peers, 'peer-b'), false);
  assert.equal(classify({ ...peers, manualConnections: [{ endpoint: 'tcp://127.0.0.1:2' }] }, 'peer-b'), undefined);
  assert.equal(classify({ manualConnections: [{ endpoint: 'tcp://127.0.0.1:2' }] }, 'peer-b'), undefined);
  const bytesRid = { toHex: () => Buffer.from('peer-a').toString('hex') };
  assert.equal(classify(peers, bytesRid), true);
});
