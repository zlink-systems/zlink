'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist');
const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { ZLinkHostSpotAddressTransport } = require('../../packages/framework/dist/runtime/host/spot-address-transport');
const { ZLinkMeshCompletionTable } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const { rawMeshBinding, rawMeshFixture, serviceNodeDescriptor } = require('./fixtures/raw-mesh-binding');

function backendFixture() {
  const { bindingPort } = rawMeshBinding();
  const node = new ZLinkNodeRawMeshBackend('mesh', 'source', bindingPort,
    new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()));
  node.setBind('inproc://source');
  node.configureObjectPlacement({
    role: 'server', placementWeight: 1, activeCapacityLimit: 10, pendingCapacityLimit: 10,
    objectCapabilities: ['object-type:settings', 'instance-spot-type:settings']
  });
  return node;
}

test('raw bind preserves the readiness supplied by its descriptor owner', () => {
  const { runtime } = rawMeshFixture({
    descriptor: serviceNodeDescriptor('source', { state: 'preparing' })
  });
  try {
    assert.equal(runtime.topology.localDescriptor().state, 'preparing');
    assert.equal(runtime.topology.selectObjectPlacementWithStatus('settings').status, 'unsupported');
  } finally {
    runtime.close();
  }
});

test('host-managed backend projects service readiness into local placement', async () => {
  const node = backendFixture();
  try {
    node.start('preparing');
    assert.equal(node.selectObjectPlacement('settings').kind, 'unsupported');
    assert.deepEqual(node.instanceSpotPlacementTypes(), []);
    await node.markServiceReady();
    assert.equal(node.selectObjectPlacement('settings').kind, 'selected');
    assert.deepEqual(node.instanceSpotPlacementTypes(), ['settings']);
  } finally {
    await node.close();
  }
});

test('standalone backend request keeps the original plain Error as its cause', async () => {
  const node = backendFixture();
  const completions = new ZLinkMeshCompletionTable();
  node.setCompletionHandler((terminal, materialize) => completions.complete(terminal, materialize));
  try {
    node.start();
    assert.equal(node.selectObjectPlacement('settings').kind, 'selected');
    const address = new ZLinkHostSpotAddressTransport({
      resolver: () => ({ resolve: async () => undefined }),
      routed: {}, meshNames: () => ['mesh'], meshNode: () => node,
      completions: () => completions, defaultRequestTimeoutMs: 1000
    });
    class Ping {}
    await assert.rejects(address.requestToSpotAddress('settings', new Ping(), {
      initialMeshName: 'mesh', instanceSpot: true, instanceSpotType: 'settings'
    }), (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
      assert.match(error.message, /result 105 and service-wire failure code 17/);
      assert.doesNotMatch(error.message, /errno/);
      assert.ok(error.cause instanceof Error);
      assert.match(error.cause.message, /Instance activation authority is not registered/);
      assert.match(error.cause.stack, /activateMissingInstance/);
      return true;
    });
  } finally {
    completions.dispose();
    await node.close();
  }
});
