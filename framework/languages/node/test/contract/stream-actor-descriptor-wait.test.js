const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const meshWire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const { withTestSessionOwners } = require('./fixtures/session-owner-descriptors');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ZLinkMeshCompletionTable
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  decodeStatefulHeader,
  encodeStatefulReply
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');

function descriptorFixture(t, resolvedGeneration = 7n) {
  let now = 1000;
  t.mock.method(performance, 'now', () => now);
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const actor = { nodeRid: 'actor-node', actorId: 'descriptor-actor', generation: 7n };
  const descriptor = (rid) => ({
    meshName: 'play',
    nodeRoutingId: rid,
    lifecycleGeneration: 3n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://${rid}`,
    channels: [],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  });
  const target = descriptor('actor-node');
  const attempts = [];
  const incoming = [];
  const routes = new Map();
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    bind() {},
    connectToRoutingId() {},
    disconnect() {},
    disconnectRid() {},
    close() {},
    localEndpoint: () => 'inproc://session-node',
    receive: () => incoming.shift(),
    send: async () => {},
    submitSend: () => ({ result: 0, admitted: Promise.resolve() }),
    connectionAttemptsSnapshot: () => [],
    routesSnapshot: () =>
      [...routes].map(([routingId, routeGeneration]) => ({ routingId, routeGeneration })),
    request: (_target, parts, _timeoutMs, onReply, onFailed, _recordReceived, onAdmitted) => {
      const record = decodeStatefulHeader(parts[0]);
      attempts.push(record);
      const tail =
        record.kind === 'actorLookup'
          ? {
              kind: 'actorLookup',
              actor: { ...actor, generation: resolvedGeneration },
              spot: { spotId: 'actor-spot', generation: 2n },
              membershipEpoch: 1n,
              authorityOwnerGeneration: 9n
            }
          : { kind: 'streamBind', bindingGeneration: 11n, authorityOwnerGeneration: 9n };
      onAdmitted?.();
      return Promise.resolve([
        encodeStatefulReply(record.correlation, RequestResult.Ok, 0, tail)
      ]).then(onReply, onFailed);
    }
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('session-node'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, close() {} }) }
  });
  raw.start();
  const runtime = withTestSessionOwners(new ServiceStatefulRuntime(raw, 'session-node', 3n));
  const backend = new ZLinkNodeRawMeshBackend(
    'play',
    'session-node',
    {},
    undefined,
    undefined,
    undefined,
    undefined,
    { stateful: runtime }
  );
  const completions = new ZLinkMeshCompletionTable();
  backend.setCompletionHandler((terminal, materialize) =>
    completions.complete(terminal, materialize)
  );
  const service = backend.createStreamSessionService({});
  service.start();
  let bindingSubmissions = 0;
  const bindActor = service.bindActor.bind(service);
  service.bindActor = (...args) => {
    bindingSubmissions++;
    return bindActor(...args);
  };
  const stream = new framework.ZLinkManagedStream(
    {},
    'session-rid',
    undefined,
    service,
    completions
  );
  return {
    attempts,
    bindingSubmissions: () => bindingSubmissions,
    bind: () =>
      stream.bindActor({ ...actor, objectGeneration: actor.generation, meshName: 'play' }, 80),
    async publish() {
      raw.expectPeerByRoutingId(target.advertisedEndpoint, target.nodeRoutingId, 'default', 3n);
      if (raw.topology.peer(target.nodeRoutingId) !== undefined) return;
      routes.set(target.nodeRoutingId, 1n);
      await raw.observeSelectedRoutes();
      incoming.push({
        sourceRid: target.nodeRoutingId,
        routeGeneration: 1n,
        parts: [meshWire.encodeRouteMeshAdmission(meshWire.M6aServiceWireCommand.admit, target)],
        close() {}
      });
      assert.equal(await raw.pumpOne(), 'infrastructure');
    },
    invalidate() {
      raw.disconnectPeer(target.advertisedEndpoint, target.nodeRoutingId);
    },
    expire() {
      now += 80;
      assert.equal(runtime.expireOperations(now, Number.POSITIVE_INFINITY), 1);
    },
    close() {
      runtime.close();
      raw.close();
      completions.dispose();
    }
  };
}

test('STREAM bind waits for a late owner descriptor and submits the binding exactly once', async (t) => {
  const fixture = descriptorFixture(t);
  try {
    const pending = fixture.bind();
    await new Promise((resolve) => setImmediate(resolve));
    assert.deepEqual(fixture.attempts, []);
    await fixture.publish();
    await pending;
    await fixture.publish();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(fixture.bindingSubmissions(), 1);
    assert.deepEqual(
      fixture.attempts.map((record) => record.kind),
      ['actorLookup', 'boundSessionBind']
    );
  } finally {
    fixture.close();
  }
});

test('STREAM bind reports DeadlineExceeded when the owner descriptor never appears', async (t) => {
  const fixture = descriptorFixture(t);
  try {
    const pending = assert.rejects(fixture.bind(), (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      return true;
    });
    await new Promise((resolve) => setImmediate(resolve));
    fixture.expire();
    await pending;
    await fixture.publish();
    assert.equal(fixture.bindingSubmissions(), 0);
    assert.deepEqual(fixture.attempts, []);
  } finally {
    fixture.close();
  }
});

test('STREAM bind reports Unavailable when the owner fence ends while awaiting its descriptor', async (t) => {
  const fixture = descriptorFixture(t);
  try {
    const pending = assert.rejects(fixture.bind(), (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
      return true;
    });
    await new Promise((resolve) => setImmediate(resolve));
    fixture.invalidate();
    await pending;
    await fixture.publish();
    assert.equal(fixture.bindingSubmissions(), 0);
    assert.deepEqual(fixture.attempts, []);
  } finally {
    fixture.close();
  }
});

test('STREAM bind still rejects a successful lookup with another Actor generation', async (t) => {
  const fixture = descriptorFixture(t, 8n);
  try {
    const pending = assert.rejects(fixture.bind(), (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.NotFound);
      return true;
    });
    await fixture.publish();
    await pending;
    assert.equal(fixture.bindingSubmissions(), 0);
    assert.deepEqual(
      fixture.attempts.map((record) => record.kind),
      ['actorLookup']
    );
  } finally {
    fixture.close();
  }
});
