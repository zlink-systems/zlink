'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');

function descriptor(nodeRoutingId) {
  return {
    meshName: 'stateful-send-admission',
    nodeRoutingId,
    lifecycleGeneration: 7n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://stateful-send-${nodeRoutingId}`,
    channels: [],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

test('remote Spot send requires ADMIT on the selected route for its owner lifecycle', async () => {
  const target = descriptor('target');
  const incoming = [];
  const sent = [];
  let routeGeneration = 9n;
  const record = (_rid, parts) => {
    sent.push(wire.decodeHeader(parts[0]).command);
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: {
      createHost: () => ({
        createRouter: () => ({
          setRoutingId() {},
          setReceiveFlowState() {},
          bind() {},
          localEndpoint: () => descriptor('source').advertisedEndpoint,
          routesSnapshot: () => [{ routingId: 'target', routeGeneration }],
          receive: () => incoming.shift(),
          send: async (rid, parts) => record(rid, parts),
          submitSend(rid, parts) {
            record(rid, parts);
            return { result: SubmitResult.Ok, admitted: Promise.resolve() };
          },
          close() {}
        }),
        close() {}
      })
    }
  });
  raw.start();
  const stateful = new ServiceStatefulRuntime(raw, 'source', 7n);
  const fence = {
    spot: { spotId: 'room', generation: 3n },
    targetNodeRid: 'target',
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 5n,
    ownerLeaseGeneration: 7n,
    storeVersion: 'version'
  };
  stateful.rememberSpotRoute(fence);
  const receive = async (frame) => {
    incoming.push({
      sourceRid: 'target',
      routeGeneration,
      parts: [frame],
      close() {}
    });
    assert.equal(await raw.pumpOne(), 'infrastructure');
  };
  const send = (route = fence) =>
    stateful.sendToSpot('source-spot', route, {
      packetName: 'Notice',
      contentType: 'application/json',
      payload: Buffer.from('{}')
    });
  try {
    await raw.observeSelectedRoutes();
    await receive(wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.hello, target));
    assert.ok(raw.topology.admittedPeer('target'));
    assert.equal(raw.isPeerRouteReady('target', 7n), false);
    const before = sent.length;
    assert.equal(await send(), SubmitResult.NotConnected, 'HELLO without ADMIT must fail');
    assert.equal(sent.length, before, 'Core must receive no application record');

    await receive(wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.admit, target));
    assert.equal(await send(), SubmitResult.Ok);
    assert.equal(sent.length, before + 1);

    const replacement = {
      ...fence,
      spot: { spotId: 'replacement-room', generation: 3n },
      targetNodeGeneration: 8n
    };
    stateful.rememberSpotRoute(replacement);
    assert.equal(await send(replacement), SubmitResult.NotConnected);
    assert.equal(sent.length, before + 1);

    routeGeneration++;
    await raw.observeSelectedRoutes();
    await receive(wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.hello, target));
    const beforeReplacementSend = sent.length;
    assert.equal(await send(), SubmitResult.NotConnected, 'ADMIT belongs to the previous route');
    assert.equal(sent.length, beforeReplacementSend);
  } finally {
    stateful.close();
    raw.close();
  }
});
