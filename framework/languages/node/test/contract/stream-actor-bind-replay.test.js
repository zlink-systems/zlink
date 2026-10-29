const { OperationRegistry } = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const { withTestSessionOwners } = require('./fixtures/session-owner-descriptors');
const {
  RequestResult, SubmitResult, ZLinkBackendResultError
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
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
  decodeStatefulHeader, encodeStatefulReply
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');

// Exercise the production sender, STREAM service, backend failure mapping and
// completion table. Transport attempts and registered completion wiring are supplied here.
function bindFixture(request, initiallyReady = true) {
  const actor = { nodeRid: 'actor-node', actorId: 'actor-bind-replay', generation: 7n };
  const attempts = [];
  let ready = initiallyReady;
  const routeObservers = new Set();
  const runtime = withTestSessionOwners(new ServiceStatefulRuntime({
    observePeerRoute: () => ({ generation: 1n, ready }),
    observePeerRouteChanged: listener => { routeObservers.add(listener); return () => routeObservers.delete(listener); },
    observePeerConnectionIntentRemoved() { return () => {}; },
    setServiceIngress() {},
    async requestService(target, parts, timeoutMs, onAdmitted) {
      const attempt = {
        target, header: Buffer.from(parts[0]), timeoutMs, at: performance.now(),
        record: decodeStatefulHeader(parts[0])
      };
      attempts.push(attempt);
      try { return await request(attempt, attempts.length, onAdmitted); }
      catch (error) {
        if (error instanceof ZLinkBackendResultError && error.operation === 'submit') ready = false;
        throw error;
      }
    }
  }, 'session-node', 3n));
  runtime.rememberActorRoute({
    actor, targetNodeGeneration: 3n, authorityOwnerGeneration: 9n, ownerLeaseGeneration: 10n
  });
  const backend = new ZLinkNodeRawMeshBackend('play', 'session-node', {}, undefined, undefined, undefined, undefined, { stateful: runtime });
  const diagnostics = [];
  const completions = new ZLinkMeshCompletionTable(d => diagnostics.push(d));
  backend.setCompletionHandler((terminal, materialize) => completions.complete(terminal, materialize));
  const service = backend.createStreamSessionService({});
  // Location resolution succeeds before the bind transport scenario starts.
  service.start();
  const lookups = new OperationRegistry();
  runtime.lookupRemoteActor = (_node, _actorId, timeoutMs) => {
    const pending = lookups.register(timeoutMs);
    queueMicrotask(() => pending.complete({
      terminalResult: RequestResult.Ok, failureCode: 0,
      kindData: { kind: 'actorLookupCompletion', location: { actor } }
    }));
    return pending;
  };
  let bindCalls = 0;
  let deadline;
  const bindActor = service.bindActor.bind(service);
  service.bindActor = (...args) => {
    bindCalls++;
    deadline = performance.now() + args[2];
    return bindActor(...args);
  };
  const stream = new framework.ZLinkManagedStream(
    { async submit() {} },
    'session-rid',
    undefined,
    service,
    completions
  );
  return {
    attempts,
    ready() { ready = true; for (const observer of routeObservers) observer('actor-node'); }, runtime, completions, diagnostics,
    bind(timeoutMs) {
      return stream.bindActor({ ...actor, objectGeneration: actor.generation, meshName: 'play' }, timeoutMs);
    },
    checkAttempts() {
      assert.equal(bindCalls, 1, 'one public bind reserves one operation');
      assert.equal(completions.pendingCount, 0);
      assert.deepEqual(diagnostics, [], 'the operation settles exactly once');
      for (const attempt of attempts) {
        assert.equal(attempt.record.kind, 'boundSessionBind');
        assert.deepEqual(attempt.header, attempts[0].header);
        assert.equal(attempt.record.correlation, attempts[0].record.correlation);
        assert.deepEqual(attempt.record.binding, attempts[0].record.binding);
        assert(attempt.timeoutMs > 0);
      }
    },
    checkDeadlines() {
      // A controlled clock makes calculation and observation share an instant.
      // Real-clock reads also include scheduling and instrumentation latency.
      for (const attempt of attempts) {
        assert(Number.isInteger(attempt.timeoutMs));
        const drift = attempt.at + attempt.timeoutMs - deadline;
        assert(drift >= 0 && drift < 1,
          `every attempt uses the whole remaining original deadline: ${JSON.stringify({
            at: attempt.at, timeoutMs: attempt.timeoutMs, deadline, drift
          })}`);
      }
    },
    close() { runtime.close(); completions.dispose(); }
  };
}

function successfulBind(attempt) {
  return [encodeStatefulReply(attempt.record.correlation, RequestResult.Ok, 0, {
    kind: 'streamBind', bindingGeneration: 11n, authorityOwnerGeneration: 9n
  })];
}

for (const wallJumpMs of [60_000, -60_000]) {
  test(`STREAM actor bind consumes initial readiness within its monotonic deadline across a ${wallJumpMs} ms wall jump`, async t => {
    let now = 1000.25;
    const wallNow = Date.now();
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const fixture = bindFixture(successfulBind, false);
    try {
      const result = fixture.bind(80);
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.attempts.length, 0);
      t.mock.method(Date, 'now', () => wallNow + wallJumpMs);
      now += 20.25;
      fixture.ready();
      await result;
      assert.deepEqual(fixture.attempts.map(a => a.timeoutMs), [60]);
      fixture.ready();
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.attempts.length, 1);
      fixture.checkAttempts();
      fixture.checkDeadlines();
      assert.deepEqual(fixture.runtime.sessionBindings('session-rid').map(b => b.bindingGeneration), [11n]);
    } finally { fixture.close(); }
  });
}

for (const started of [false, true]) {
  test(`STREAM actor bind registry expiry is DeadlineExceeded (started=${started})`, async t => {
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    let finish;
    const fixture = bindFixture(attempt => new Promise(resolve => {
      finish = () => resolve(successfulBind(attempt));
    }), started);
    try {
      const operation = assert.rejects(fixture.bind(80), error => {
        assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
        return true;
      });
      await new Promise(resolve => setImmediate(resolve));
      now += 80;
      assert.equal(fixture.runtime.expireOperations(now, Number.POSITIVE_INFINITY), 1);
      await operation;
      finish?.();
      fixture.ready();
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.attempts.length, started ? 1 : 0);
      assert.equal(fixture.runtime.allSessionBindings().length, 0);
      fixture.checkAttempts();
    } finally { fixture.close(); }
  });
}

for (const phase of ['request', 'submit']) {
  test(`STREAM actor bind returns first ${phase} NOT_CONNECTED once`, async () => {
    const fixture = bindFixture(() => {
      throw new ZLinkBackendResultError(phase,
        phase === 'request' ? RequestResult.NotConnected : SubmitResult.NotConnected);
    });
    try {
      await assert.rejects(fixture.bind(80), error => {
        assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
        return true;
      });
      fixture.ready();
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.attempts.length, 1);
      assert.equal(fixture.runtime.allSessionBindings().length, 0);
      fixture.checkAttempts();
    } finally { fixture.close(); }
  });
}

test('STREAM actor bind stops on a received failure envelope or malformed reply', async () => {
  for (const [reply, expectedKind] of [
    [a => [encodeStatefulReply(a.record.correlation, RequestResult.Rejected, 15)],
      framework.ZLinkFrameworkErrorKind.Rejected],
    [() => [Buffer.from('invalid reply')], framework.ZLinkFrameworkErrorKind.ProtocolError]
  ]) {
    const fixture = bindFixture(reply);
    try {
      await assert.rejects(fixture.bind(80), error => {
        assert.equal(error.kind, expectedKind);
        return true;
      });
      fixture.ready();
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.attempts.length, 1);
      fixture.checkAttempts();
    } finally { fixture.close(); }
  }
});
