const assert = require('node:assert/strict');
const test = require('node:test');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

for (const endpointOnly of [false, true]) {
  for (const hostAttached of [false, true]) {
    test(`NotRequired exchanges both descriptors before retiring the intent (endpointOnly=${endpointOnly}, hostAttached=${hostAttached})`, async () => {
      const pair = createPair({ endpointOnly, hostAttached, notify: true });
      const { left, right, disconnected, sent } = pair;
      try {
        // The acceptor's Hello reaches the connector before the acceptor has
        // consumed any descriptor. Closing here discards the queued response.
        assert.equal(await right.pumpOne(), 'infrastructure');
        assert.equal(await left.pumpOne(), 'infrastructure');
        assert.equal(await left.pumpOne(), 'infrastructure');
        assert.equal(left.topology.notRequiredPeers().length, 1);
        assert.equal(right.topology.notRequiredPeers().length, 0);
        assert.deepEqual(disconnected, [], 'Hello alone must not terminate the outbound intent');
        const response = sent.at(-1);
        assert.equal(wire.decodeHeader(response.parts[0]).command, wire.M6aServiceWireCommand.admit);
        assert.deepEqual(
          wire.decodeRouteMeshAdmission(response.parts[0], wire.M6aServiceWireCommand.admit, 'left'),
          left.topology.localDescriptor()
        );

        assert.equal(await right.pumpOne(), 'infrastructure');
        assert.deepEqual(disconnected, []);
        assert.equal(await left.pumpOne(), 'infrastructure');
        await assertTerminalPair(pair);
      } finally {
        left.close();
        right.close();
      }
    });
  }
}

test('NotRequired Admit conveys the peer descriptor before the connector closes', async () => {
  const pair = createPair({ endpointOnly: false, hostAttached: false, notify: false });
  const { left, right, disconnected } = pair;
  try {
    assert.equal(await left.observeSelectedRoutes(), 1);
    assert.equal(await right.pumpOne(), 'infrastructure');
    assert.equal(right.topology.notRequiredPeers().length, 1);
    assert.equal(left.topology.notRequiredPeers().length, 0);
    assert.deepEqual(disconnected, []);
    assert.equal(await left.pumpOne(), 'infrastructure');
    await assertTerminalPair(pair);
  } finally {
    left.close();
    right.close();
  }
});

async function assertTerminalPair({ left, right, disconnected, sent, selectRoute }) {
  assert.deepEqual(left.topology.notRequiredPeers(), [right.topology.localDescriptor()]);
  assert.deepEqual(right.topology.notRequiredPeers(), [left.topology.localDescriptor()]);
  assert.equal(left.topology.peers().length, 0);
  assert.equal(right.topology.peers().length, 0);
  assert.equal(left.liveness.size, 0);
  assert.equal(right.liveness.size, 0);
  assert.deepEqual(disconnected, [right.topology.localDescriptor().advertisedEndpoint]);

  // A later selected route cannot revive a terminal intent (MeshNode §7.1).
  const sentBeforeReady = sent.length;
  selectRoute(17n);
  assert.equal(await left.observeSelectedRoutes(), 1);
  assert.equal(await left.announceExpectedPeers(), 0);
  assert.equal(await right.announceExpectedPeers(), 0);
  assert.equal(await left.announcePeer('right'), false);
  assert.equal(sent.length, sentBeforeReady);
  assert.deepEqual(disconnected, [right.topology.localDescriptor().advertisedEndpoint]);
  assert.equal(left.topology.notRequiredPeers().length, 1);
  assert.equal(right.topology.notRequiredPeers().length, 1);
}

test('HELLO is submitted once per selected route before admission', async () => {
  const pair = createPair({ endpointOnly: false, hostAttached: false, notify: false });
  const { left, right, sent, selectRoute } = pair;
  try {
    assert.equal(await left.observeSelectedRoutes(), 1);
    assert.equal(sent.length, 1);
    await left.pumpBatch(false);
    await left.pumpBatch(false);
    assert.equal(sent.length, 1, 'idle pumps must preserve the accepted HELLO');
    assert.equal(await left.announcePeer('right'), false);
    const descriptor = right.topology.localDescriptor();
    left.connectPeer(descriptor.advertisedEndpoint, descriptor);
    left.connectPeerByRoutingId(descriptor.advertisedEndpoint, descriptor.nodeRoutingId);
    left.expectPeerByRoutingId(descriptor.advertisedEndpoint, descriptor.nodeRoutingId);
    assert.equal(await left.announceExpectedPeers(), 0);
    assert.equal(sent.length, 1, 'intent refresh must preserve the accepted HELLO');
    selectRoute(17n);
    assert.equal(await left.observeSelectedRoutes(), 1);
    assert.equal(sent.length, 2, 'a replacement route needs one new HELLO');
    await left.pumpBatch(false);
    assert.equal(sent.length, 2);
    left.disconnectPeer(right.topology.localDescriptor().advertisedEndpoint, 'right');
    left.connectPeer(
      right.topology.localDescriptor().advertisedEndpoint,
      right.topology.localDescriptor()
    );
    await left.observeSelectedRoutes();
    await left.announceExpectedPeers();
    assert.equal(sent.length, 3, 'a removed intent must forget its accepted HELLO');
  } finally {
    left.close();
    right.close();
  }
});

test('a rejected HELLO does not record acceptance for its selected route', async () => {
  const pair = createPair({ endpointOnly: false, hostAttached: false, notify: false });
  const { left, right, sent, rejectNextSubmit } = pair;
  try {
    rejectNextSubmit();
    await left.observeSelectedRoutes();
    assert.equal(sent.length, 0);
    assert.equal(await left.announceExpectedPeers(), 1);
    assert.equal(sent.length, 1);
    assert.equal(await left.announceExpectedPeers(), 0);
    assert.equal(sent.length, 1);
  } finally {
    left.close();
    right.close();
  }
});

test('late HELLO acceptance belongs to the route generation submitted', async () => {
  const pair = createPair({ endpointOnly: false, hostAttached: false, notify: false });
  const { left, right, sent, selectRoute, holdNextSubmit } = pair;
  try {
    const accept = holdNextSubmit();
    const oldObservation = left.observeSelectedRoutes();
    selectRoute(17n);
    await left.observeSelectedRoutes();
    accept();
    await oldObservation;
    assert.equal(sent.length, 2);
    assert.equal(await left.announceExpectedPeers(), 0);
    assert.equal(sent.length, 2, 'old acceptance must not overwrite the new route acceptance');
  } finally {
    left.close();
    right.close();
  }
});

function createPair({ endpointOnly, hostAttached, notify }) {
  const queues = { left: [], right: [] };
  let routeGeneration = 1n;
  const disconnected = [];
  const sent = [];
  let connected = false;
  let rejectSubmit = false;
  let heldSubmit;
  const descriptors = Object.fromEntries(['left', 'right'].map((rid, index) => [rid, {
    meshName: 'not-required-mesh', nodeRoutingId: rid,
    lifecycleGeneration: BigInt(index + 41), descriptorRevision: 7n,
    advertisedEndpoint: `inproc://not-required-${rid}`, channels: [], state: 'preparing',
    securityIdentity: 'default', applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'client',
    placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 16,
    activeCapacityUsed: 0, pendingCapacityUsed: 0
  }]));
  function runtime(rid) {
    const remote = rid === 'left' ? 'right' : 'left';
    const connect = endpoint => {
      assert.equal(endpoint, descriptors[remote].advertisedEndpoint);
      connected = true;
      if (notify) {
        queues.left.push({ sourceRid: 'right', routeGeneration, parts: [] });
        queues.right.push({ sourceRid: 'left', routeGeneration, parts: [] });
      }
    };
    const router = {
      setRoutingId(value) { assert.equal(value, rid); },
      bind(endpoint) { assert.equal(endpoint, descriptors[rid].advertisedEndpoint); },
      localEndpoint() { return descriptors[rid].advertisedEndpoint; },
      setReceiveFlowState() {},
      connect,
      connectToRoutingId(target, endpoint) { assert.equal(target, remote); connect(endpoint); },
      disconnect(endpoint) {
        disconnected.push(endpoint);
        connected = false;
        // Submit acceptance does not guarantee delivery before disconnect.
        queues.left.length = 0;
        queues.right.length = 0;
      },
      async send(target, parts) {
        assert.equal(target, remote);
        assert.equal(connected, true);
        if (rejectSubmit) {
          rejectSubmit = false;
          throw new Error('Controlled submit refusal');
        }
        const record = { sourceRid: rid, routeGeneration, parts: parts.map(part => Buffer.from(part)) };
        if (heldSubmit !== undefined) {
          const held = heldSubmit;
          heldSubmit = undefined;
          await held;
        }
        sent.push(record);
        queues[remote].push(record);
      },
      receive() {
        const record = queues[rid].shift();
        return record === undefined ? undefined : {
          ...record, sourceRoute: Buffer.from(record.sourceRid), close() {}
        };
      },
      routesSnapshot() {
        return connected ? [{ routingId: remote, routeGeneration }] : [];
      },
      close() {}
    };
    return new RawServiceMeshRuntime({
      descriptor: descriptors[rid],
      applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
      ...(hostAttached ? { peerAdmissionSealed: () => false } : {}),
      bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
    });
  }
  const left = runtime('left');
  const right = runtime('right');
  left.start();
  right.start();
  if (endpointOnly) left.connectPeerEndpoint(descriptors.right.advertisedEndpoint);
  else left.connectPeer(descriptors.right.advertisedEndpoint, descriptors.right);
  const selectRoute = generation => {
    routeGeneration = generation;
    connected = true;
  };
  return { left, right, sent, disconnected, selectRoute,
    rejectNextSubmit() { rejectSubmit = true; },
    holdNextSubmit() {
      let accept;
      heldSubmit = new Promise(resolve => { accept = resolve; });
      return accept;
    }
  };
}
