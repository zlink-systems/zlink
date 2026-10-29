const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
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
const { randomOperationId } = require('../../packages/framework/dist/runtime/locations/creation-operation-id');
const { withTestSessionOwners } = require('./fixtures/session-owner-descriptors');

const operationKinds = ['userSpotCreate', 'userSpotClose', 'actorCreate', 'streamBind', 'actorJoin'];
const turn = () => new Promise(resolve => setImmediate(resolve));

function fixture(request, disconnectOnFailure = true) {
  const descriptor = rid => ({
    meshName: 'durable-lifecycle', nodeRoutingId: rid, lifecycleGeneration: 7n,
    descriptorRevision: 1n, advertisedEndpoint: `inproc://${rid}`, channels: [], state: 'serving',
    securityIdentity: 'default', applicationVersion: 1n, protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server', placementWeight: 100, activeCapacityLimit: 100,
    pendingCapacityLimit: 10, activeCapacityUsed: 0, pendingCapacityUsed: 0
  });
  const target = descriptor('target-node');
  const incoming = [];
  const routes = new Map();
  let nextRouteGeneration = 1n;
  const attempts = [];
  const router = {
    setRoutingId() {}, setReceiveFlowState() {}, bind() {}, connectToRoutingId() {}, connect() {},
    disconnect() {}, disconnectRid() {}, close() {}, localEndpoint: () => 'inproc://source-node',
    receive: () => incoming.shift(), send: async () => {},
    submitSend: () => ({ result: SubmitResult.Ok, admitted: Promise.resolve() }),
    routesSnapshot: () => [...routes].map(([routingId, routeGeneration]) => ({ routingId, routeGeneration })),
    request: (rid, parts, timeoutMs, onReply, onFailed, _recordReceived, onAdmitted, existing) => (async () => {
      const record = parts[0][3] === 28 ? decodeActorJoin28(parts) : wire.decodeStatefulHeader(parts[0]);
      const attempt = { rid, parts, timeoutMs, record, existing };
      attempts.push(attempt);
      try {
        const result = request(attempt, attempts.length);
        onAdmitted?.();
        return await result;
      } catch (error) {
        if (disconnectOnFailure && error instanceof ZLinkBackendResultError) {
          routes.delete(target.nodeRoutingId);
          await raw.observeSelectedRoutes();
        }
        throw error;
      }
    })().then(onReply, onFailed)
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source-node'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, close() {} }) }
  });
  raw.start();
  raw.connectPeerByRoutingId(target.advertisedEndpoint, target.nodeRoutingId, 'default', 7n);
  const runtime = withTestSessionOwners(new ServiceStatefulRuntime(raw, 'source-node', 7n));
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
  return {
    raw, runtime, attempts, target, router,
    async admit() {
      const routeGeneration = nextRouteGeneration++;
      routes.set(target.nodeRoutingId, routeGeneration);
      await raw.observeSelectedRoutes();
      incoming.push({ sourceRid: target.nodeRoutingId, routeGeneration,
        parts: [meshWire.encodeRouteMeshAdmission(meshWire.M6aServiceWireCommand.admit, target)], close() {} });
      assert.equal(await raw.pumpOne(), 'infrastructure');
    },
    async disconnectTransport() {
      routes.delete(target.nodeRoutingId);
      await raw.observeSelectedRoutes();
      assert.equal(raw.topology.peer(target.nodeRoutingId), undefined);
    },
    start(kind, timeoutMs = 80) {
      const common = { sourceNodeRid: 'source-node', sourceNodeGeneration: 7n,
        deadlineUnixMs: BigInt(Date.now() + timeoutMs) };
      if (kind === 'userSpotCreate') return runtime.requestUserSpotCreate('target-node',
        { ...common, spotId: 'spot', stableType: 'Room', reservation }, timeoutMs);
      if (kind === 'actorCreate') return runtime.requestActorCreate('target-node',
        { ...common, operation: randomOperationId(), actorId: 'actor', stableType: 'Player', reservation }, timeoutMs);
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
  for (const mode of ['inFlight', 'beforeStart']) {
    test(`${kind}: intent removal immediately terminates ${mode} as Unavailable`, async t => {
      let now = 1000;
      t.mock.method(performance, 'now', () => now);
      t.mock.timers.enable({ apis: ['setTimeout'] });
      let finishAttempt;
      const f = fixture(() => new Promise(resolve => { finishAttempt = resolve; }));
      try {
        if (mode === 'inFlight') await f.admit();
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
        assert.equal(f.attempts.length, mode === 'inFlight' ? 1 : 0, 'terminal operation cannot submit again');
        assert.equal(f.raw[inspectRuntime]().peerConnectionIntentObservers, 0, 'terminal releases the transition observer');
      } finally { f.close(); }
    });
  }

  test(`${kind}: physical disconnect preserves the original request until logical intent removal`, async t => {
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    let reply;
    const f = fixture(() => new Promise(resolve => { reply = resolve; }));
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
      assert.equal(f.attempts.length, 1, 'absent routes receive no submission');
      f.raw.disconnectPeerEndpoint(f.target.advertisedEndpoint);
      await turn();
      assert.equal(outcome?.error?.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
      assert.match(outcome.error.message, /lifecycle ended/);
      await operation;
      reply(f.terminal(f.attempts[0]));
      await turn();
      assert.equal(f.raw[inspectRuntime]().peerConnectionIntentObservers, 0);
    } finally { f.close(); }
  });

  test(`${kind}: first binding failure terminates once without replay`, async t => {
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
      const f = fixture((attempt, count) => {
        if (count === 1) throw failure;
        return f.terminal(attempt);
      });
      try {
        await f.admit();
        let outcome;
        let completions = 0;
        const operation = f.start(kind).then(value => { completions++; outcome = { value }; }, error => { completions++; outcome = { error }; });
        await turn();
        if (failure instanceof ZLinkBackendResultError && failure.operation === 'request' &&
            failure.result === RequestResult.TimedOut) {
          assert(outcome?.error instanceof framework.ZLinkFrameworkException);
          assert.equal(outcome.error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
          assert.equal(outcome.error.message, 'Request exceeded its deadline.');
        } else {
          assert.equal(outcome?.error, failure, 'non-timeout binding failure retains original identity');
        }
        const fixedOutcome = outcome;
        await f.admit();
        await turn();
        assert.equal(f.attempts.length, 1, 'a new ready route cannot retry the binding failure');
        assert.equal(outcome, fixedOutcome);
        assert.equal(completions, 1);
        await operation;
        assert.equal(f.raw[inspectRuntime]().peerConnectionIntentObservers, 0);
      } finally { f.close(); }
    }
  });

  test(`${kind}: malformed terminal envelope ends replay as ProtocolError`, async () => {
    const f = fixture(() => [Buffer.from('malformed')]);
    try {
      await f.admit();
      await assert.rejects(f.start(kind), error => error.kind === framework.ZLinkFrameworkErrorKind.ProtocolError);
      assert.equal(f.attempts.length, 1);
      assert.equal(f.raw[inspectRuntime]().peerConnectionIntentObservers, 0);
    } finally { f.close(); }
  });
}

test('removing a stale lifecycle intent does not terminate the admitted replacement', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  let reply;
  const f = fixture(() => new Promise(resolve => { reply = resolve; }), false);
  try {
    await f.admit();
    const operation = f.start('actorCreate');
    await turn();
    f.raw.disconnectPeer(f.target.advertisedEndpoint, f.target.nodeRoutingId, 6n);
    assert(f.raw.topology.peer(f.target.nodeRoutingId));
    t.mock.timers.tick(20);
    reply(f.terminal(f.attempts[0]));
    assert.equal((await operation).terminalResult, RequestResult.Rejected);
    assert.equal(f.attempts.length, 1);
  } finally { f.close(); }
});

test('raw binding request maps the installed binding failure through the existing typed backend translation', async () => {
  const host = new ZLinkNodeRawBindingPort().createHost();
  const router = host.createRouter();
  try {
    router.setRoutingId('typed-source');
    router.bind('tcp://127.0.0.1:0');
    await assert.rejects(new Promise((resolve, reject) => router.request('absent-peer', [Buffer.from('request')], 20, resolve, reject)), error => {
      assert(error instanceof ZLinkBackendResultError);
      assert(error.cause instanceof zlink.SubmitError);
      assert.equal(error.operation, 'submit');
      assert.equal(error.result, SubmitResult.NotConnected);
      return true;
    });
  } finally { host.close(); }
});

for (const kind of operationKinds) {
  test(`${kind}: a new selected ready route preserves the original physical request`, async t => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    let originalReply;
    let outcome;
    const f = fixture((attempt, count) => count === 1
      ? new Promise(resolve => { originalReply = () => resolve(f.terminal(attempt)); })
      : f.terminal(attempt));
    try {
      await f.admit();
      const operation = f.start(kind).then(value => { outcome = value; }, error => { outcome = { error }; });
      await turn();
      assert.equal(f.attempts.length, 1);
      await f.raw.observeSelectedRoutes();
      await turn();
      assert.equal(f.attempts.length, 1, 'unchanged selection does not replay');
      await f.admit();
      await turn();
      assert.equal(f.attempts.length, 1, 'new ready selection cannot resubmit a started request');
      assert.equal(outcome, undefined, 'original physical reply remains pending');
      originalReply();
      await operation;
      assert.equal(outcome?.terminalResult, RequestResult.Rejected);
      assert.equal(f.attempts.length, 1);
    } finally { f.close(); }
  });
}

for (const started of [false, true]) {
  test(`standalone infrastructure direct request uses the existing raw registry (started=${started})`, async t => {
    const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    let finish;
    const f = fixture(() => new Promise(resolve => { finish = resolve; }));
    try {
      if (started) await f.admit();
      let timers = 0;
      const setTimer = globalThis.setTimeout;
      t.mock.method(globalThis, 'setTimeout', (...args) => { timers++; return setTimer(...args); });
      const backend = new ZLinkNodeRawMeshBackend('mesh', 'source-node', {}, undefined,
        undefined, undefined, undefined, { raw: f.raw });
      const frames = [wire.encodeUserSpotCloseHeader({
        sourceNodeRid: 'source-node', sourceNodeGeneration: 7n, correlation: 77n,
        operation: { high: 7n, low: 77n }, deadlineUnixMs: BigInt(Date.now() + 80),
        target: { spotId: 'spot', objectGeneration: 3n, targetNodeRid: 'target-node',
          targetNodeGeneration: 7n, authorityOwnerGeneration: 5n, expectedStoreVersion: 'version' }
      })];
      let completions = 0;
      const caller = backend.requestInfrastructureControlFrames('target-node', frames, { timeoutMs: 80 });
      const outcome = assert.rejects(caller, error => {
        completions++;
        assert(error instanceof framework.ZLinkFrameworkException);
        assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
        return true;
      });
      await turn();
      assert.equal(f.raw.pendingOperationCount, 1);
      assert.equal(f.runtime.pendingOperationCount, 0);
      assert.equal(f.attempts.length, started ? 1 : 0);
      assert.equal(timers, 0, 'the existing raw visitor owns expiry without a request timer');
      if (started) assert.equal(f.attempts[0].existing.deadlineMs, 1080);
      now += 80;
      assert.equal(f.raw.expireOperations(now, Number.POSITIVE_INFINITY), 1);
      await outcome;
      assert.equal(f.raw.pendingOperationCount, 0);
      finish?.(f.terminal(f.attempts[0]));
      await f.admit();
      await turn();
      assert.equal(f.attempts.length, started ? 1 : 0);
      assert.equal(completions, 1);
      assert.equal(f.raw[inspectRuntime]().peerConnectionIntentObservers, 0);
    } finally { f.close(); }
  });
}


test('Raw Promise request preserves the physical Promise and rejected-Promise submit semantics', async t => {
  const f = fixture(() => undefined);
  let resolve;
  const physical = new Promise(yes => { resolve = yes; });
  const cause = new Error('synchronous binding refusal');
  let hooks = 0;
  try {
    await f.admit();
    t.mock.method(f.router, 'request', () => physical);
    const reply = f.raw.requestService('target-node', [], 80, undefined, undefined, undefined, () => hooks++);
    assert.equal(reply, physical);
    assert.equal(hooks, 1);
    resolve([]); await reply;
    t.mock.method(f.router, 'request', () => { throw cause; });
    let refused;
    assert.doesNotThrow(() => { refused = f.raw.requestService('target-node', [], 80, undefined, undefined, undefined, () => hooks++); });
    await assert.rejects(refused, error => error === cause);
    assert.equal(hooks, 1);
  } finally { f.runtime.close(); f.raw.close(); }
});

for (const mode of ['sync', 'async', 'cancel-at-creation']) {
  test(`submitted hook failure uses original pending and retains late physical decoder/mode=${mode}`, async t => {
    const { RequestRuntime } = require('../../packages/framework/dist/runtime/foundation/request-runtime');
    const { requestDurableService } = require('../../packages/framework/dist/runtime/foundation/durable-service-request');
    const f = fixture(() => undefined);
    const requests = new RequestRuntime();
    const pending = requests.begin(80);
    const cause = new Error('original submitted hook failure');
    const controller = new AbortController();
    let resolve, hooks = 0, decoded = 0, submitted = 0;
    const physical = new Promise(yes => { resolve = yes; });
    try {
      await f.admit();
      t.mock.method(f.router, 'request', () => { submitted++; if (mode === 'cancel-at-creation') controller.abort(cause); return physical; });
      const caller = requests.run(pending, receiver => {
        void requestDurableService(f.raw, 'target-node', [], pending, 'test', controller.signal, signal => {
          hooks++;
          if (mode === 'sync') throw cause;
          if (mode === 'cancel-at-creation') { assert.equal(signal.aborted, true); return Promise.resolve(); }
          return Promise.reject(cause);
        }).then(value => receiver.operationCompleted(pending.id, value, undefined), error => receiver.operationFailed(pending.id, error, undefined));
      }, raw => { decoded++; return raw; }, error => error);
      assert.equal(hooks, 1, 'start bookkeeping runs before an async callback boundary');
      await assert.rejects(caller, error => error === cause);
      assert.equal(decoded, 0);
      resolve([Buffer.from('late physical reply')]);
      await turn();
      assert.equal(decoded, 1);
      assert.equal(hooks, 1);
      assert.equal(submitted, 1);
      assert.equal(requests.registry.size, 0);
    } finally { resolve([]); requests.close(); f.runtime.close(); f.raw.close(); }
  });
}
