const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { RequestResult, SubmitResult, ZLinkBackendResultError } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { ZLinkNodeRawBindingPort } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const { SERVICE_WIRE_REQUIRED_CAPABILITY } = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const meshWire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const { decodeActorJoin28 } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');

const operationKinds = ['userSpotCreate', 'userSpotClose', 'actorCreate', 'streamBind', 'actorJoin'];
const turn = () => new Promise(resolve => setImmediate(resolve));

function fixture(request) {
  const descriptor = rid => ({
    meshName: 'durable-lifecycle', nodeRoutingId: rid, lifecycleGeneration: 7n,
    descriptorRevision: 1n, advertisedEndpoint: `inproc://${rid}`, channels: [], state: 'serving',
    securityIdentity: 'default', applicationVersion: 1n, protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server', placementWeight: 100, activeCapacityLimit: 100,
    pendingCapacityLimit: 10, activeCapacityUsed: 0, pendingCapacityUsed: 0
  });
  const target = descriptor('target-node');
  const incoming = [];
  const events = [];
  const attempts = [];
  const router = {
    setRoutingId() {}, setReceiveFlowState() {}, bind() {}, connectToRoutingId() {}, connect() {},
    disconnect() {}, disconnectRid() {}, close() {}, localEndpoint: () => 'inproc://source-node',
    receive: () => incoming.shift(), send: async () => {},
    monitor: () => ({ statusReady: () => true, close() {}, drain(handler) {
      const batch = events.splice(0);
      for (const event of batch) handler(event);
      return batch.length;
    } }),
    request: async (rid, parts, timeoutMs) => {
      const record = parts[0][3] === 28 ? decodeActorJoin28(parts) : wire.decodeStatefulHeader(parts[0]);
      const attempt = { rid, parts, timeoutMs, record };
      attempts.push(attempt);
      return request(attempt, attempts.length);
    }
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source-node'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, close() {} }) }
  });
  raw.start();
  raw.connectPeerByRoutingId(target.advertisedEndpoint, target.nodeRoutingId, 'default', 7n);
  const runtime = new ServiceStatefulRuntime(raw, 'source-node', 7n);
  const actor = { nodeRid: 'target-node', actorId: 'actor', generation: 3n };
  const spot = { spotId: 'spot', generation: 3n };
  runtime.rememberActorRoute({ actor, targetNodeGeneration: 7n, authorityOwnerGeneration: 5n, ownerLeaseGeneration: 7n });
  runtime.rememberSpotRoute({ spot, targetNodeRid: 'target-node', targetNodeGeneration: 7n,
    authorityOwnerGeneration: 5n, ownerLeaseGeneration: 7n, storeVersion: 'version' });
  const reservation = {
    reservationId: 'reservation', expectedStoreVersion: 'version', objectGeneration: 3n,
    authorityOwnerGeneration: 5n, targetNodeRid: 'target-node', targetNodeGeneration: 7n,
    targetOwnerId: 'target-owner', targetOwnerLeaseGeneration: 7n, pendingCapacityDelta: 1
  };
  const monitor = event => ({ event, value: 1n, flags: 1, routingId: 'target-node', connectionId: 1n,
    localAddress: 'inproc://source-node', remoteAddress: target.advertisedEndpoint });
  return {
    raw, runtime, attempts, target,
    async admit() {
      events.push(monitor(0x1000));
      await raw.drainMonitorEvents();
      incoming.push({ sourceRid: target.nodeRoutingId,
        parts: [meshWire.encodeRouteMeshAdmission(meshWire.M6aServiceWireCommand.hello, target)], close() {} });
      assert.equal(await raw.pumpOne(), 'infrastructure');
    },
    async disconnectTransport() {
      events.push(monitor(0x0200));
      await raw.drainMonitorEvents();
      assert.equal(raw.topology.peer(target.nodeRoutingId), undefined);
    },
    start(kind, timeoutMs = 80) {
      const common = { sourceNodeRid: 'source-node', sourceNodeGeneration: 7n,
        deadlineUnixMs: BigInt(Date.now() + timeoutMs) };
      if (kind === 'userSpotCreate') return runtime.requestUserSpotCreate('target-node',
        { ...common, spotId: 'spot', stableType: 'Room', reservation }, timeoutMs);
      if (kind === 'actorCreate') return runtime.requestActorCreate('target-node',
        { ...common, actorId: 'actor', stableType: 'Player', reservation }, timeoutMs);
      if (kind === 'userSpotClose') return runtime.requestUserSpotClose('target-node', { ...common,
        target: { spotId: 'spot', objectGeneration: 3n, targetNodeRid: 'target-node',
          targetNodeGeneration: 7n, authorityOwnerGeneration: 5n, expectedStoreVersion: 'version' }
      }, timeoutMs);
      if (kind === 'streamBind') return runtime.bindSession('session', actor, timeoutMs, async () => true).promise;
      return runtime.joinActor(actor, 'target-node', spot, spot.generation, undefined, timeoutMs).promise;
    },
    terminal(attempt) {
      return [wire.encodeStatefulReply(attempt.record.correlation, RequestResult.Rejected, 15)];
    },
    close() { runtime.close(); raw.close(); }
  };
}

for (const kind of operationKinds) {
  for (const mode of ['inFlight', 'replayDelay']) {
    test(`${kind}: intent removal immediately terminates ${mode} as Unavailable`, async t => {
      let now = 1000;
      t.mock.method(performance, 'now', () => now);
      t.mock.timers.enable({ apis: ['setTimeout'] });
      let finishAttempt;
      const f = fixture(() => mode === 'inFlight'
        ? new Promise(resolve => { finishAttempt = resolve; })
        : Promise.reject(new ZLinkBackendResultError('submit', SubmitResult.NotConnected)));
      try {
        let outcome;
        const operation = f.start(kind).then(value => { outcome = { value }; }, error => { outcome = { error }; });
        await turn();
        assert.equal(outcome, undefined);
        f.raw.disconnectPeer(f.target.advertisedEndpoint, f.target.nodeRoutingId, 7n);
        await turn();
        assert.equal(outcome?.error?.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
        assert.match(outcome.error.message, /lifecycle ended/);
        assert.equal(now, 1000, 'termination does not consume a timer tick');
        await operation;
        finishAttempt?.(f.terminal(f.attempts[0]));
        now += 100;
        t.mock.timers.tick(100);
        await turn();
        assert.equal(f.attempts.length, 1, 'late transport completion cannot restart a terminal operation');
        assert.equal(f.raw.peerConnectionIntentRemoved.size, 0, 'terminal releases the transition observer');
      } finally { f.close(); }
    });
  }

  test(`${kind}: physical disconnect retains replay until the logical endpoint intent is removed`, async t => {
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const f = fixture(() => Promise.reject(new ZLinkBackendResultError('request', RequestResult.NotConnected)));
    try {
      await f.admit();
      let outcome;
      const operation = f.start(kind).then(value => { outcome = { value }; }, error => { outcome = { error }; });
      await turn();
      await f.disconnectTransport();
      now += 20;
      t.mock.timers.tick(20);
      await turn();
      assert.equal(outcome, undefined, 'peer absence alone is transient');
      assert.equal(f.attempts.length, 2);
      f.raw.disconnectPeerEndpoint(f.target.advertisedEndpoint);
      await turn();
      assert.equal(outcome?.error?.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
      assert.match(outcome.error.message, /lifecycle ended/);
      await operation;
      assert.equal(f.raw.peerConnectionIntentRemoved.size, 0);
    } finally { f.close(); }
  });

  test(`${kind}: only typed transient transport errors replay`, async t => {
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const transient = [
      ...[SubmitResult.NotConnected, SubmitResult.NotFound, SubmitResult.NotAdmitted, SubmitResult.Backpressured]
        .map(result => new ZLinkBackendResultError('submit', result)),
      ...[RequestResult.NotConnected, RequestResult.TimedOut]
        .map(result => new ZLinkBackendResultError('request', result))
    ];
    const terminal = [
      new TypeError('encode failed'), new meshWire.ServiceWireProtocolError('invalid wire'),
      new zlink.ConfigError(zlink.ConfigResult.InvalidArgument, 22),
      new ZLinkBackendResultError('request', RequestResult.ProtocolError),
      new ZLinkBackendResultError('submit', SubmitResult.InvalidArgument),
      Object.assign(new Error('untyped result lookalike'), { operation: 'request', result: RequestResult.TimedOut })
    ];
    for (const failure of [...transient, ...terminal]) {
      const replay = transient.includes(failure);
      const f = fixture((attempt, count) => {
        if (count === 1) throw failure;
        return f.terminal(attempt);
      });
      try {
        let outcome;
        const operation = f.start(kind).then(value => { outcome = { value }; }, error => { outcome = { error }; });
        await turn();
        if (replay) {
          assert.equal(outcome, undefined);
          now += 20;
          t.mock.timers.tick(20);
          await turn();
          assert.equal(outcome?.value?.terminalResult, RequestResult.Rejected);
          assert.equal(f.attempts.length, 2);
          assert.deepEqual(f.attempts[1].parts, f.attempts[0].parts);
        } else {
          assert.equal(outcome?.error, failure, 'non-replayable failure retains its original identity');
          assert.equal(f.attempts.length, 1);
        }
        await operation;
        assert.equal(f.raw.peerConnectionIntentRemoved.size, 0);
      } finally { f.close(); }
    }
  });

  test(`${kind}: malformed terminal envelope ends replay as ProtocolError`, async () => {
    const f = fixture(() => [Buffer.from('malformed')]);
    try {
      await assert.rejects(f.start(kind), error => error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError);
      assert.equal(f.attempts.length, 1);
      assert.equal(f.raw.peerConnectionIntentRemoved.size, 0);
    } finally { f.close(); }
  });
}

test('removing a stale lifecycle intent does not terminate the admitted replacement', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const f = fixture((attempt, count) => {
    if (count === 1) throw new ZLinkBackendResultError('request', RequestResult.NotConnected);
    return f.terminal(attempt);
  });
  try {
    await f.admit();
    const operation = f.start('actorCreate');
    await turn();
    f.raw.disconnectPeer(f.target.advertisedEndpoint, f.target.nodeRoutingId, 6n);
    assert(f.raw.topology.peer(f.target.nodeRoutingId));
    t.mock.timers.tick(20);
    assert.equal((await operation).terminalResult, RequestResult.Rejected);
    assert.equal(f.attempts.length, 2);
  } finally { f.close(); }
});

test('raw binding request maps the installed binding failure through the existing typed backend translation', async () => {
  const host = new ZLinkNodeRawBindingPort().createHost();
  const router = host.createRouter();
  try {
    router.setRoutingId('typed-source');
    router.bind('tcp://127.0.0.1:0');
    await assert.rejects(router.request('absent-peer', [Buffer.from('request')], 20), error => {
      assert(error instanceof ZLinkBackendResultError);
      assert(error.cause instanceof zlink.SubmitError);
      assert.equal(error.operation, 'submit');
      assert.equal(error.result, SubmitResult.NotConnected);
      return true;
    });
  } finally { host.close(); }
});
