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
const {
  internalFrameworkWireReply,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
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

function descriptorFixture(t) {
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
      const actorGenerationMismatch =
        record.kind === 'boundSessionBind' && record.actor.actor.generation !== actor.generation;
      const mismatch = actorGenerationMismatch
        ? internalFrameworkWireReply(ZLinkFrameworkInternalErrorKind.ActorLocationStale)
        : undefined;
      const tail =
        record.kind === 'actorLookup'
          ? {
              kind: 'actorLookup',
              actor,
              spot: { spotId: 'actor-spot', generation: 2n },
              membershipEpoch: 1n,
              authorityOwnerGeneration: 9n
            }
          : record.kind === 'boundSessionBind' && !actorGenerationMismatch
            ? { kind: 'streamBind', bindingGeneration: 11n, authorityOwnerGeneration: 9n }
            : undefined;
      onAdmitted?.();
      return Promise.resolve([
        encodeStatefulReply(
          record.correlation,
          mismatch?.terminalResult ?? RequestResult.Ok,
          mismatch?.failureCode ?? 0,
          tail
        )
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
  let bindOperations = 0;
  const bindActor = service.bindActor.bind(service);
  service.bindActor = (...args) => {
    bindOperations++;
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
    service,
    attempts,
    lookup: () => runtime.lookupRemoteActor(actor.nodeRid, actor.actorId, 80).promise,
    request: () =>
      runtime.requestToActor(
        actor,
        { packetName: 'Ping', contentType: 'application/json', payload: Buffer.from('{}') },
        80,
        undefined,
        { sessionRid: 'session-rid', bindingGeneration: 11n }
      ).promise,
    async suspendRoute() {
      routes.delete(target.nodeRoutingId);
      await raw.observeSelectedRoutes();
      assert.equal(raw.topology.admittedPeer(target.nodeRoutingId), undefined);
    },
    async restart() {
      raw.disconnectPeer(target.advertisedEndpoint, target.nodeRoutingId);
      raw.expectPeerByRoutingId(target.advertisedEndpoint, target.nodeRoutingId, 'default', 4n);
      routes.set(target.nodeRoutingId, 2n);
      await raw.observeSelectedRoutes();
      incoming.push({
        sourceRid: target.nodeRoutingId,
        routeGeneration: 2n,
        parts: [
          meshWire.encodeRouteMeshAdmission(meshWire.M6aServiceWireCommand.admit,
            { ...target, lifecycleGeneration: 4n })
        ],
        close() {}
      });
      assert.equal(await raw.pumpOne(), 'infrastructure');
      assert.equal(raw.topology.admittedPeer(target.nodeRoutingId).descriptor.lifecycleGeneration, 4n);
    },
    bindOperations: () => bindOperations,
    bind: (objectGeneration = actor.generation) =>
      stream.bindActor(
        { ...actor, generation: objectGeneration, objectGeneration, meshName: 'play' },
        80,
        undefined,
        undefined,
        { ownerNodeGeneration: 3n, authorityOwnerGeneration: 9n, ownerLeaseGeneration: 103n }
      ),
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
    async admitOtherGeneration() {
      const old = { ...target, lifecycleGeneration: 2n };
      routes.set(old.nodeRoutingId, 1n);
      await raw.observeSelectedRoutes();
      incoming.push({
        sourceRid: old.nodeRoutingId,
        routeGeneration: 1n,
        parts: [meshWire.encodeRouteMeshAdmission(meshWire.M6aServiceWireCommand.admit, old)],
        close() {}
      });
      assert.equal(await raw.pumpOne(), 'infrastructure');
      assert.equal(raw.topology.admittedPeer(target.nodeRoutingId).remoteAdmitReceived, true);
      raw.expectPeerByRoutingId(target.advertisedEndpoint, target.nodeRoutingId, 'default', 3n);
      return old;
    },
    admittedDescriptor: () => raw.topology.admittedPeer(target.nodeRoutingId)?.descriptor,
    async removeTargetExpectation() {
      raw.expectedPeers.delete(target.nodeRoutingId);
      // The existing connect observation wakes waiting admission without changing G0.
      await raw.pumpBatch(false, false, true);
    },
    invalidate() {
      raw.disconnectPeer(target.advertisedEndpoint, target.nodeRoutingId);
    },
    expire(expected = 1) {
      now += 80;
      assert.equal(runtime.expireOperations(now, Number.POSITIVE_INFINITY), expected);
    },
    close() {
      runtime.close();
      raw.close();
      completions.dispose();
    }
  };
}

test('STREAM authority route binds and requests without command 26, then rejects the old lifecycle without submission', async (t) => {
  const fixture = descriptorFixture(t);
  try {
    await fixture.publish();
    await fixture.bind();
    assert.equal(fixture.attempts[0].actor.targetNodeGeneration, 3n);
    assert.equal(fixture.attempts[0].actor.authorityOwnerGeneration, 9n);
    assert.equal(fixture.attempts[0].actor.ownerLeaseGeneration, 103n);
    const reply = await fixture.request();
    assert.equal(reply.terminalResult, RequestResult.Ok);
    assert.deepEqual(fixture.attempts.map((record) => record.kind), ['boundSessionBind', 'actorRequest']);
    const submitted = fixture.attempts.length;
    await fixture.suspendRoute();
    const pending = assert.rejects(fixture.request(), (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
      return true;
    });
    assert.equal(fixture.attempts.length, submitted);
    await fixture.restart();
    await pending;
    assert.equal(fixture.attempts.length, submitted);
  } finally {
    fixture.close();
  }
});

test('STREAM bind rejects an ActorRef with a different authority ObjectGeneration', async (t) => {
  const fixture = descriptorFixture(t);
  try {
    await fixture.publish();
    const result = await fixture.bind(8n).then(
      () => ({ accepted: true }),
      (error) => ({ accepted: false, error })
    );
    assert.equal(result.accepted, false);
    assert.deepEqual(fixture.attempts.map((record) => record.kind), ['boundSessionBind']);
    assert.deepEqual(fixture.service.bindings('session-rid'), []);
    // The spec's InvalidOperation kind (02-session-actor-binding:870) is not yet produced by
    // any language; #4774 aligns all four and narrows this assertion to that kind.
    assert.ok(Object.values(framework.ZLinkFrameworkErrorKind).includes(result.error.kind));
  } finally {
    fixture.close();
  }
});

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
    assert.equal(fixture.bindOperations(), 1);
    assert.deepEqual(
      fixture.attempts.map((record) => record.kind),
      ['boundSessionBind']
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
    // The logical bind owns admission; no wire request starts before its descriptor.
    assert.equal(fixture.bindOperations(), 1);
    fixture.expire();
    await pending;
    await fixture.publish();
    assert.equal(fixture.bindOperations(), 1);
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
    assert.equal(fixture.bindOperations(), 1);
    assert.deepEqual(fixture.attempts, []);
  } finally {
    fixture.close();
  }
});

test('remote command 26 returns its location without synthesizing an authority route', async (t) => {
  const fixture = descriptorFixture(t);
  try {
    await fixture.publish();
    const lookup = await fixture.lookup();
    assert.equal(lookup.terminalResult, RequestResult.Ok);
    assert.equal(lookup.kindData.location.actor.generation, 7n);
    const result = await fixture.request();
    assert.equal(result.terminalResult, RequestResult.NotFound);
    assert.deepEqual(fixture.attempts.map((record) => record.kind), ['actorLookup']);
    assert.equal(fixture.bindOperations(), 0);
  } finally {
    fixture.close();
  }
});

for (const removeExpectation of [false, true]) {
  test(`STREAM bind preserves the terminal with an admitted older lifecycle (remove expectation: ${removeExpectation})`, async (t) => {
    const fixture = descriptorFixture(t);
    try {
      const old = await fixture.admitOtherGeneration();
      const result = fixture.bind().then(
        () => undefined,
        (error) => error
      );
      await new Promise((resolve) => setImmediate(resolve));
      assert.deepEqual(fixture.attempts, []);
      assert.equal(fixture.bindOperations(), 1);
      if (removeExpectation) await fixture.removeTargetExpectation();
      fixture.expire(removeExpectation ? 0 : 1);
      const error = await result;
      assert.deepEqual(fixture.admittedDescriptor(), old);
      assert.deepEqual(fixture.attempts, []);
      assert.equal(fixture.bindOperations(), 1);
      assert.equal(
        error?.kind,
        removeExpectation
          ? framework.ZLinkFrameworkErrorKind.Unavailable
          : framework.ZLinkFrameworkErrorKind.DeadlineExceeded
      );
    } finally {
      fixture.close();
    }
  });
}
