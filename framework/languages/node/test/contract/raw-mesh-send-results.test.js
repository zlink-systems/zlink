const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const { ZLinkBackendResultError, SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const { createServiceWireCodec } = require('../../packages/framework/dist/runtime/foundation/service-wire-codec');
const codec = createServiceWireCodec({ magic: wire.M6A_SERVICE_WIRE_MAGIC, major: wire.M6A_SERVICE_WIRE_MAJOR, commands: wire.M6aServiceWireCommand });

function fixture() {
  const descriptor = rid => ({
    meshName: 'send-results', nodeRoutingId: rid, lifecycleGeneration: 1n, descriptorRevision: 1n,
    advertisedEndpoint: `inproc://send-results-${rid}`, channels: [{ name: 'channel', weight: 100 }],
    state: 'serving', securityIdentity: 'default', applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
    placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 16,
    activeCapacityUsed: 0, pendingCapacityUsed: 0
  });
  const received = [];
  const sent = [];
  let failure;
  const router = {
    setRoutingId() {}, bind() {}, close() {}, setReceiveFlowState() {}, localEndpoint() { return descriptor("local").advertisedEndpoint; },
    submitSend(target, parts) {
      sent.push({ target, command: wire.decodeHeader(parts[0]).command });
      if (failure !== undefined) {
        const error = failure;
        failure = undefined;
        if (error.phase === 'completion') {
          return { result: SubmitResult.Backpressured, admitted: Promise.reject(error) };
        }
        throw error;
      }
      return { result: SubmitResult.Ok, admitted: Promise.resolve() };
    },
    async send(target, parts) {
      sent.push({ target, command: wire.decodeHeader(parts[0]).command });
      if (failure !== undefined) { const error = failure; failure = undefined; throw error; }
    },
    receive() { return received.shift(); }, routesSnapshot() { return []; }
  };
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  runtime.start();
  return {
    runtime, sent,
    fail(error) { failure = error; },
    enqueue(rid, parts) { received.push({ sourceRid: rid, sourceRoute: Buffer.from(rid), routeGeneration: 1n, parts, close() {} }); },
    async admit(rid) {
      this.enqueue(rid, [wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.hello, descriptor(rid))]);
      assert.equal(await runtime.pumpOne(0), 'infrastructure');
      assert.ok(runtime.topology.peer(rid));
    },
    descriptor
  };
}

for (const result of [SubmitResult.NotConnected, SubmitResult.NotFound]) {
  for (const phase of ['submit', 'completion']) {
    const routeError = () => new ZLinkBackendResultError('submit', result, undefined, { phase });
    for (const response of ['hello', 'reject', 'admit', 'livenessAck']) {
      test(`route absence ${result}/${phase} completes received ${response} without exception`, async () => {
        const f = fixture();
        try {
          if (response === 'livenessAck') await f.admit('peer');
          const parts = response === 'hello' ? [] : response === 'livenessAck'
            ? [codec.encodeLivenessRecord({ command: wire.M6aServiceWireCommand.livenessProbe, probeId: 1n })]
            : [wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.hello, {
                ...f.descriptor('peer'), ...(response === 'reject' ? { meshName: 'wrong-mesh' } : {})
              })];
          f.enqueue('peer', parts);
          f.fail(routeError());
          // Backpressured is submitted; only immediate route rejection drops a response.
          assert.equal(await f.runtime.pumpOne(0), phase === 'submit' && ['hello', 'livenessAck'].includes(response) ? 'dropped' : 'infrastructure');
        } finally { f.runtime.close(); }
      });
    }
    test(`route absence ${result}/${phase} preserves descriptor updates to later peers and channel result`, async () => {
      const f = fixture();
      try {
        await f.admit('first'); await f.admit('second');
        f.sent.length = 0;
        f.fail(routeError());
        await f.runtime.updateLocalDescriptor({ placementWeight: 50 });
        assert.deepEqual(f.sent.map(record => record.target), ['first', 'second']);
        const tick = await f.runtime.tickLiveness(0);
        for (const probe of tick.probes) {
          f.enqueue(probe.nodeRoutingId, [codec.encodeLivenessRecord({ command: wire.M6aServiceWireCommand.livenessAck, probeId: probe.probeId })]);
          assert.equal(await f.runtime.pumpOne(1), 'infrastructure');
        }
        assert.equal(f.runtime.isPeerRouteReady('first'), true);
        f.fail(routeError());
        assert.equal(await f.runtime.sendToChannel('channel', Buffer.from('payload')), SubmitResult.NotConnected);
      } finally { f.runtime.close(); }
    });
  }
}

test('non-route submission failures propagate from receive and update paths', async () => {
  for (const cause of [new Error('adapter failure'), new ZLinkBackendResultError('submit', SubmitResult.Terminated, undefined, { phase: 'completion' })]) {
    const f = fixture();
    try {
      f.enqueue('peer', []); f.fail(cause);
      if (cause.phase === 'completion') {
        assert.equal(await f.runtime.pumpOne(0), 'infrastructure');
        await turn();
        await assert.rejects(f.runtime.pumpBatch(false), error => error === cause);
      } else {
        await assert.rejects(f.runtime.pumpOne(0), error => error === cause);
      }
      await f.admit('peer'); f.fail(cause);
      await assert.rejects(f.runtime.updateLocalDescriptor({ placementWeight: 50 }), error => error === cause);
    } finally { f.runtime.close(); }
  }
});


test('route absence during a liveness probe does not prevent probes to later peers', async () => {
  const f = fixture();
  try {
    await f.admit('first'); await f.admit('second');
    f.sent.length = 0;
    f.fail(new ZLinkBackendResultError('submit', SubmitResult.NotFound, undefined, { phase: 'completion' }));
    const tick = await f.runtime.tickLiveness(0);
    assert.equal(tick.probes.length, 2);
    assert.deepEqual(f.sent.map(record => record.target), ['first', 'second']);
  } finally { f.runtime.close(); }
});
