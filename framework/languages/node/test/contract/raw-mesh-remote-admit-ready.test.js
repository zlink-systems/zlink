const assert = require('node:assert/strict');
const test = require('node:test');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');

const PROBE_INTERVAL_MS = 100;

function descriptor(nodeRoutingId) {
  return {
    meshName: 'hello-cadence',
    nodeRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://hello-cadence-${nodeRoutingId}`,
    channels: [{ name: 'alpha', weight: 100 }],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 16,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

function fixture(deferred) {
  const sent = [];
  const requested = [];
  let complete;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local'),
    probeIntervalMs: PROBE_INTERVAL_MS,
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: {
      createHost: () => ({
        createRouter: () => ({
          setRoutingId() {},
          setReceiveFlowState() {},
          setReadableHandler() {},
          bind() {},
          localEndpoint: () => descriptor('local').advertisedEndpoint,
          routesSnapshot: () => [{ routingId: 'peer', routeGeneration: 11n }],
          submitSend(target, parts) {
            const command = wire.decodeHeader(parts[0]).command;
            sent.push(command);
            if (deferred && command === wire.M6aServiceWireCommand.hello) {
              return {
                result: SubmitResult.Backpressured,
                admitted: new Promise((resolve) => {
                  complete = resolve;
                })
              };
            }
            return { result: SubmitResult.Ok, admitted: Promise.resolve() };
          },
          request(target) {
            requested.push(target);
            throw new Error('Core request reached');
          },
          close() {}
        }),
        shutdown() {},
        close() {}
      })
    }
  });
  runtime.start();
  return {
    runtime,
    sent,
    requested,
    hellos: () => sent.filter((command) => command === wire.M6aServiceWireCommand.hello).length,
    async completeHello() {
      const pending = [...runtime.controlSends.values()].find(
        (exchange) => exchange.hello !== undefined
      )?.task;
      complete?.();
      await pending;
    },
    receive(command) {
      return runtime.processReceived(
        {
          sourceRid: 'peer',
          sourceRoute: Buffer.from('peer'),
          routeGeneration: 11n,
          parts: [wire.encodeRouteMeshAdmission(command, descriptor('peer'))],
          close() {}
        },
        performance.now()
      );
    }
  };
}

const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

for (const deferred of [false, true]) {
  test(`peer readiness requires inbound ADMIT (deferred HELLO=${deferred})`, async (t) => {
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    const f = fixture(deferred);
    const backend = Object.create(ZLinkNodeRawMeshBackend.prototype);
    backend.runtime = f.runtime;
    backend.peerIntents = new Map();
    const readyTransitions = [];
    const unobserve = f.runtime.observePeerRouteChanged(() => {
      if (f.runtime.isPeerRouteReady('peer')) readyTransitions.push(true);
    });
    try {
      await f.runtime.observeSelectedRoutes();
      await f.completeHello();
      await f.receive(wire.M6aServiceWireCommand.hello);
      assert.ok(
        f.sent.includes(wire.M6aServiceWireCommand.admit),
        'local ADMIT submission succeeds'
      );
      assert.ok(
        f.runtime.topology.admittedPeer('peer'),
        'local admission remains available to ingress'
      );
      assert.equal(f.runtime.isPeerRouteReady('peer'), false);
      assert.deepEqual(f.runtime.observePeerRoute('peer'), { generation: 11n, ready: false });
      assert.equal(backend.status().admittedPeerCount, 0);
      assert.equal(f.runtime.topology.selectChannel('alpha'), undefined);
      assert.equal(f.runtime.topology.selectPlacement(), undefined);
      const before = f.sent.length;
      assert.equal(await f.runtime.sendToNode('peer', Buffer.from('payload')), false);
      assert.equal(
        await f.runtime.sendToChannel('alpha', Buffer.from('payload')),
        SubmitResult.NotConnected
      );
      const request = f.runtime.requestToNode('peer', Buffer.from('payload'), 1000);
      assert.equal((await request.promise).terminalResult, RequestResult.NotFound);
      assert.equal(f.sent.length, before, 'not-ready application records never reach Core');
      assert.deepEqual(f.requested, []);
      for (let period = 1; period <= 2; period++) {
        now += PROBE_INTERVAL_MS;
        f.runtime.tickLiveness(now);
        await f.completeHello();
        assert.equal(f.hellos(), period + 1);
      }
      await f.receive(wire.M6aServiceWireCommand.admit);
      assert.equal(f.runtime.isPeerRouteReady('peer', 1n), true);
      assert.equal(f.runtime.isPeerRouteReady('peer', 2n), false);
      assert.deepEqual(f.runtime.observePeerRoute('peer'), { generation: 11n, ready: true });
      assert.equal(backend.status().admittedPeerCount, 1);
      assert.equal(f.runtime.topology.selectChannel('alpha').descriptor.nodeRoutingId, 'peer');
      assert.equal(f.runtime.topology.selectPlacement().descriptor.nodeRoutingId, 'peer');
      assert.equal(await f.runtime.sendToNode('peer', Buffer.from('payload')), true);
      await assert.rejects(
        f.runtime.requestToNode('peer', Buffer.from('payload'), 1000).promise,
        /Core request reached/
      );
      assert.deepEqual(f.requested, ['peer']);
      await f.receive(wire.M6aServiceWireCommand.admit);
      await f.receive(wire.M6aServiceWireCommand.hello);
      assert.equal(backend.status().admittedPeerCount, 1);
      assert.equal(
        readyTransitions.length,
        1,
        'duplicate admission does not publish readiness twice'
      );
      for (let period = 0; period < 2; period++) {
        now += PROBE_INTERVAL_MS;
        f.runtime.tickLiveness(now);
        assert.equal(f.hellos(), 3, 'remote ADMIT ends HELLO retransmission');
      }
    } finally {
      unobserve();
      await f.completeHello();
      f.runtime.close();
    }
  });
}

test('object placement and route replacement use the same inbound ADMIT fact', () => {
  const {
    ServiceTopologyRegistry
  } = require('../../packages/framework/dist/runtime/foundation/service-topology-registry');
  const topology = new ServiceTopologyRegistry(descriptor('local'));
  const peer = {
    ...descriptor('peer'),
    protocolCapabilities: [...descriptor('peer').protocolCapabilities, 'object-type:T'].sort()
  };
  const receive = (command, route) => topology.admit(peer, route, undefined, undefined, command);
  assert.equal(receive(wire.M6aServiceWireCommand.hello, 'route:11'), 'admitted');
  assert.equal(topology.selectObjectPlacementWithStatus('T').status, 'unavailable');
  assert.equal(
    topology.selectObjectPlacement('T', () => true),
    undefined
  );
  assert.equal(receive(wire.M6aServiceWireCommand.admit, 'route:11'), 'admitted');
  assert.equal(topology.selectObjectPlacement('T').nodeRoutingId, 'peer');
  assert.equal(receive(wire.M6aServiceWireCommand.hello, 'route:12'), 'admitted');
  assert.equal(topology.hasRemoteAdmit('peer', 'route:12'), false);
  assert.equal(topology.selectObjectPlacementWithStatus('T').status, 'unavailable');
  assert.equal(
    topology.selectChannel('alpha', () => true),
    undefined
  );
  assert.equal(
    topology.selectPlacement(() => true),
    undefined
  );
  assert.equal(receive(wire.M6aServiceWireCommand.admit, 'route:12'), 'admitted');
  assert.equal(topology.selectObjectPlacement('T').nodeRoutingId, 'peer');
});

test('local HELLO admission permits inbound liveness before remote ADMIT', async () => {
  const {
    createServiceWireCodec
  } = require('../../packages/framework/dist/runtime/foundation/service-wire-codec');
  const codec = createServiceWireCodec({
    magic: wire.M6A_SERVICE_WIRE_MAGIC,
    major: wire.M6A_SERVICE_WIRE_MAJOR,
    commands: wire.M6aServiceWireCommand
  });
  const f = fixture(false);
  try {
    await f.runtime.observeSelectedRoutes();
    await f.receive(wire.M6aServiceWireCommand.hello);
    assert.equal(f.runtime.isPeerRouteReady('peer'), false);
    assert.equal(
      await f.runtime.processReceived(
        {
          sourceRid: 'peer',
          sourceRoute: Buffer.from('peer'),
          routeGeneration: 11n,
          parts: [
            codec.encodeLivenessRecord({
              command: wire.M6aServiceWireCommand.livenessProbe,
              probeId: 1n
            })
          ],
          close() {}
        },
        performance.now()
      ),
      'infrastructure'
    );
    assert.equal(f.sent.at(-1), wire.M6aServiceWireCommand.livenessAck);
    assert.equal(f.runtime.isPeerRouteReady('peer'), false);
  } finally {
    f.runtime.close();
  }
});
