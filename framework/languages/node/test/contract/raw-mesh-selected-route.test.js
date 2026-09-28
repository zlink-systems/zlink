const assert = require('node:assert/strict');
const test = require('node:test');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const { RequestResult, ZLinkBackendResultError } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

//  Core ROUTER §10.1 selects one route per RID; the Framework observes that
//  selection through its snapshot (transport liveness §5).

function descriptor(nodeRoutingId) {
  return {
    meshName: 'selected-route-mesh', nodeRoutingId,
    lifecycleGeneration: 1n, descriptorRevision: 1n,
    advertisedEndpoint: `inproc://selected-route-${nodeRoutingId}`,
    channels: [{ name: 'alpha', weight: 100 }], state: 'serving',
    securityIdentity: 'default', applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
    placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 16,
    activeCapacityUsed: 0, pendingCapacityUsed: 0
  };
}

function selectedRouteRuntime(routes) {
  const sent = [];
  const requested = [];
  let monitorOpened = false;
  const router = {
    setRoutingId() {}, setReceiveFlowState() {}, setReadableHandler() {}, bind() {},
    localEndpoint: () => 'inproc://selected-route-local',
    connect() {}, connectToRoutingId() {}, disconnect() {}, disconnectRid() {},
    monitor() {
      monitorOpened = true;
      return { drain: () => 0, statusReady: () => true, close() {} };
    },
    routesSnapshot: () => [...routes].map(([routingId, routeGeneration]) => ({ routingId, routeGeneration })),
    async send(target, parts) {
      sent.push([target, parts[0][3]]);
    },
    async request(target) {
      //  Core decides whether the selected route carries the request.
      requested.push(target);
      throw new ZLinkBackendResultError('request', RequestResult.Busy);
    },
    receive: () => undefined,
    close() {}
  };
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  runtime.start();
  return {
    runtime, sent, requested,
    monitorOpened: () => monitorOpened,
    processReceived: record => runtime.processReceived(
      { ...record, sourceRoute: Buffer.from(record.sourceRid), close() {} },
      performance.now(),
      undefined
    )
  };
}

test('RouteMesh admission follows the observed Core selected route (FW01)', async () => {
  const peer = descriptor('peer');
  const routes = new Map([['peer', 11n]]);
  const harness = selectedRouteRuntime(routes);
  const { runtime, sent, processReceived } = harness;
  const hello = routeGeneration => processReceived({
    sourceRid: 'peer', routeGeneration,
    parts: [Buffer.from(wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.hello, peer))]
  });
  try {
    runtime.connectPeerByRoutingId(peer.advertisedEndpoint, 'peer');
    await runtime.pumpBatch(false);

    // Core delivers the handshake of the selected route.
    assert.equal(await hello(11n), 'infrastructure');
    assert.equal(runtime.topology.peer('peer')?.connectionId, 'route:11');

    // Core replaces the route: the admission ends and the new route handshakes.
    routes.set('peer', 12n);
    sent.length = 0;
    await runtime.pumpBatch(false);
    assert.equal(runtime.topology.peer('peer'), undefined);
    assert.ok(sent.some(([target, command]) =>
      target === 'peer' && command === wire.M6aServiceWireCommand.hello));
    assert.equal(await hello(12n), 'infrastructure');
    assert.equal(runtime.topology.peer('peer')?.connectionId, 'route:12');

    // The route disappears: nothing stays admitted on it.
    routes.delete('peer');
    await runtime.pumpBatch(false);
    assert.equal(runtime.topology.peer('peer'), undefined);
    assert.equal(runtime.isPeerRouteReady('peer'), false);

    // Route state never came from a ROUTER monitor.
    assert.equal(harness.monitorOpened(), false);
  } finally {
    runtime.close();
  }
});

test('a direct RID request selects only the logical target and leaves route acceptance to Core (FW02)', async () => {
  const peer = descriptor('peer');
  const { runtime, requested } = selectedRouteRuntime(new Map([['peer', 21n]]));
  const payload = {
    packetName: 'DirectQuestion', contentType: 'application/json', payload: Buffer.from('question')
  };
  try {
    // An RID that is not an admitted peer is no target: the requester's selection.
    assert.deepEqual(await runtime.requestToNode('unknown', payload, 1_000).promise,
      { terminalResult: RequestResult.NotFound, failureCode: 14 });
    assert.equal(runtime.topology.admit(peer, 'route:20'), 'admitted');
    assert.equal(runtime.topology.disconnect('peer', 'route:20'), true);
    assert.notEqual(runtime.topology.knownDescriptor('peer'), undefined);
    assert.deepEqual(await runtime.requestToNode('peer', payload, 1_000).promise,
      { terminalResult: RequestResult.NotFound, failureCode: 14 });
    assert.deepEqual(requested, []);

    // An admitted target is submitted to Core even before its first liveness
    // ACK; Core's REQUEST result is the terminal, not a Framework copy.
    assert.equal(runtime.topology.admit(peer, 'route:21'), 'admitted');
    runtime.liveness.admit('peer', 'route:21', performance.now());
    assert.equal(runtime.isPeerRouteReady('peer'), false);
    await assert.rejects(runtime.requestToNode('peer', payload, 1_000).promise, error =>
      error instanceof ZLinkBackendResultError
      && error.operation === 'request'
      && error.result === RequestResult.Busy);
    assert.deepEqual(requested, ['peer']);
  } finally {
    runtime.close();
  }
});
