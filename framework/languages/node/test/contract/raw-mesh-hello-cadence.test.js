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
