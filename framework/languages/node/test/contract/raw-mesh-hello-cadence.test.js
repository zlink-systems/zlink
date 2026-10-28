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

function fixture(deferred, options = {}) {
  const sent = [];
  const received = [];
  let complete;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local'),
    probeIntervalMs: PROBE_INTERVAL_MS,
    peerAdmissionSealed: options.peerAdmissionSealed,
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: {
      createHost: () => ({
        createRouter: () => ({
          receive: () => received.shift(),
          setRoutingId() {},
          setReceiveFlowState() {},
          setReadableHandler() {},
          bind() {},
          localEndpoint: () => descriptor('local').advertisedEndpoint,
          routesSnapshot: () => options.routes ?? [{ routingId: 'peer', routeGeneration: 11n }],
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
    hellos: () => sent.filter((command) => command === wire.M6aServiceWireCommand.hello).length,
    async completeHello() {
      complete?.();
      await new Promise(setImmediate);
    },
    receive(command) {
      received.push({
        sourceRid: 'peer', sourceRoute: Buffer.from('peer'), routeGeneration: 11n,
        parts: [wire.encodeRouteMeshAdmission(command, descriptor('peer'))], close() {}
      });
      return runtime.pumpOne();
    },
    receiveRecord(record) {
      received.push(record);
      return runtime.pumpOne();
    }
  };
}

// MeshNode §7.1: readiness rotations between management deadlines must not resend HELLO.
for (const deferred of [false, true]) {
  for (const crossedHello of [false, true]) {
    test(`HELLO awaiting ADMIT follows the probe cadence (deferred=${deferred}, crossedHello=${crossedHello})`, async (t) => {
      let now = 1000;
      t.mock.method(performance, 'now', () => now);
      const f = fixture(deferred);
      try {
        await f.runtime.observeSelectedRoutes();
        await f.completeHello();
        if (crossedHello) await f.receive(wire.M6aServiceWireCommand.hello);
        f.runtime.tickLiveness(now);
        assert.equal(f.hellos(), 1, 'the initial HELLO already covers this management period');
        for (let period = 1; period <= 5; period++) {
          const due = 1000 + period * PROBE_INTERVAL_MS;
          for (const time of [due - PROBE_INTERVAL_MS + 1, due - 1]) {
            now = time;
            assert.equal(f.runtime.nextMaintenanceDeadlineMs(now), due);
            f.runtime.tickLiveness(now);
            assert.equal(f.hellos(), period);
          }
          now = due;
          assert.equal(f.runtime.nextMaintenanceDeadlineMs(now), due);
          f.runtime.tickLiveness(now);
          await f.completeHello();
          assert.equal(f.hellos(), period + 1, 'exactly one HELLO per liveness period');
          assert.equal(f.runtime.nextMaintenanceDeadlineMs(now), due + PROBE_INTERVAL_MS);
        }
        await f.receive(wire.M6aServiceWireCommand.admit);
        now += PROBE_INTERVAL_MS;
        f.runtime.tickLiveness(now);
        assert.equal(f.hellos(), 6, 'ADMIT ends HELLO retransmission');
      } finally {
        await f.completeHello();
        f.runtime.close();
      }
    });
  }
}

test('a pending HELLO submission waits for completion readiness', async (t) => {
  let now = 1000;
  t.mock.method(performance, 'now', () => now);
  const f = fixture(true);
  try {
    await f.runtime.observeSelectedRoutes();
    now += PROBE_INTERVAL_MS;
    f.runtime.tickLiveness(now);
    assert.equal(f.hellos(), 1);
    assert.ok(
      f.runtime.nextMaintenanceDeadlineMs(now) > now,
      'an outstanding Core submission must not force another receive rotation'
    );
    await f.completeHello();
    now += PROBE_INTERVAL_MS;
    f.runtime.tickLiveness(now);
    assert.equal(f.hellos(), 2, 'completion permits the next due HELLO');
  } finally {
    await f.completeHello();
    f.runtime.close();
  }
});

for (const resume of ['observation', 'management']) {
  test(`a sealed route submits its first HELLO before the retry deadline on ${resume}`, async (t) => {
    let sealed = true;
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    const f = fixture(false, { peerAdmissionSealed: () => sealed });
    try {
      await f.runtime.observeSelectedRoutes();
      assert.equal(f.hellos(), 0, 'the admission seal still blocks new handshakes');
      sealed = false;
      if (resume === 'observation') await f.runtime.observeSelectedRoutes();
      else f.runtime.tickLiveness(1000);
      assert.equal(
        f.hellos(),
        1,
        'first HELLO is submitted without advancing the management clock'
      );
      await f.runtime.observeSelectedRoutes();
      f.runtime.tickLiveness(1000);
      assert.equal(f.hellos(), 1, 'an unchanged route does not duplicate its initial submission');
      for (let period = 1; period <= 2; period++) {
        now += PROBE_INTERVAL_MS;
        f.runtime.tickLiveness(now);
        assert.equal(f.hellos(), period + 1, 'submitted HELLO follows the existing retry cadence');
      }
      await f.receive(wire.M6aServiceWireCommand.admit);
      now += PROBE_INTERVAL_MS;
      f.runtime.tickLiveness(now);
      assert.equal(f.hellos(), 3, 'remote ADMIT ends retransmission after the first HELLO resumes');
    } finally {
      f.runtime.close();
    }
  });
}

test('route observation submits one initial HELLO per selected generation before management', async (t) => {
  t.mock.method(performance, 'now', () => 1000);
  const routes = [{ routingId: 'peer', routeGeneration: 11n }];
  const f = fixture(false, { routes });
  try {
    await f.runtime.observeSelectedRoutes();
    assert.equal(f.hellos(), 1);
    await f.runtime.observeSelectedRoutes();
    assert.equal(f.hellos(), 1);
    await f.receive(wire.M6aServiceWireCommand.admit);
    routes[0] = { routingId: 'peer', routeGeneration: 12n };
    await f.runtime.observeSelectedRoutes();
    assert.equal(f.hellos(), 2, 'replacement begins a new handshake immediately');
    await f.runtime.observeSelectedRoutes();
    assert.equal(f.hellos(), 2);
  } finally {
    f.runtime.close();
  }
});
