const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
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
const {
  OperationKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

// Exercise the production sender, STREAM service, backend failure mapping and
// completion table. Only transport attempts and ready dispatch are supplied here.
function bindFixture(request) {
  const actor = { nodeRid: 'actor-node', actorId: 'actor-bind-replay', generation: 7n };
  const attempts = [];
  const runtime = new ServiceStatefulRuntime({
    setServiceIngress() {},
    async requestService(target, parts, timeoutMs) {
      const attempt = {
        target, header: Buffer.from(parts[0]), timeoutMs, at: performance.now(),
        record: decodeStatefulHeader(parts[0])
      };
      attempts.push(attempt);
      return request(attempt, attempts.length);
    }
  }, 'session-node', 3n);
  runtime.rememberActorRoute({
    actor, targetNodeGeneration: 3n, authorityOwnerGeneration: 9n, ownerLeaseGeneration: 10n
  });
  const backend = new ZLinkNodeRawMeshBackend('play', 'session-node', {});
  backend.stateful = runtime;
  const diagnostics = [];
  const completions = new ZLinkMeshCompletionTable(undefined, d => diagnostics.push(d));
  backend.readyHandler = () => queueMicrotask(() => {
    let completion;
    while ((completion = backend.takeCompletion()) !== undefined) {
      completions.complete({
        operationId: completion.operationId,
        operationKind: completion.operationKind,
        terminalResult: completion.result.terminalResult,
        failureErrno: completion.result.failureCode,
        kindData: completion.result.kindData ?? null,
        parts: []
      });
    }
  });
  const service = backend.createStreamSessionService({});
  // Location resolution succeeds before the bind transport scenario starts.
  service.lookupActor = () => backend.observeStateful(OperationKind.ActorLookup, {
    id: 100n,
    promise: Promise.resolve({
      terminalResult: RequestResult.Ok, failureCode: 0,
      kindData: { kind: 'actorLookupCompletion', location: { actor } }
    })
  });
  let bindCalls = 0;
  let deadline;
  const bindActor = service.bindActor.bind(service);
  service.bindActor = (...args) => {
    bindCalls++;
    deadline = performance.now() + args[2];
    return bindActor(...args);
  };
  const stream = new framework.ZLinkManagedStream({}, 'session-rid', undefined, service, completions);
  return {
    attempts, runtime, completions, diagnostics,
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
  test(`STREAM actor bind: each replay uses the remaining monotonic deadline across a ${wallJumpMs} ms wall jump`, async t => {
    let now = 1000.25;
    const wallNow = Date.now();
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const fixture = bindFixture(() => {
      throw new ZLinkBackendResultError('submit', SubmitResult.NotConnected);
    });
    try {
      const result = assert.rejects(fixture.bind(80), error => {
        assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
        return true;
      });
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.attempts.length, 1);
      t.mock.method(Date, 'now', () => wallNow + wallJumpMs);
      // Include fractional remaining budgets, then exhaust the original deadline.
      for (const elapsed of [20.25, 20.25, 20.25, 19.25]) {
        now += elapsed;
        t.mock.timers.tick(elapsed);
        await new Promise(resolve => setImmediate(resolve));
      }
      await result;
      assert.deepEqual(fixture.attempts.map(a => a.timeoutMs), [80, 60, 40, 20]);
      fixture.checkAttempts();
      fixture.checkDeadlines();
      assert.equal(fixture.runtime.allSessionBindings().length, 0);
    } finally { fixture.close(); }
  });
}

test('STREAM actor bind: route absent for the whole deadline is Unavailable with no ingress', async () => {
  let ingress = 0;
  const fixture = bindFixture(async () => {
    throw new ZLinkBackendResultError('submit', SubmitResult.NotConnected);
  });
  try {
    await assert.rejects(fixture.bind(80), error => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
      return true;
    });
    assert.equal(ingress, 0);
    assert(fixture.attempts.length > 1);
    assert.equal(fixture.runtime.allSessionBindings().length, 0);
    fixture.checkAttempts();
  } finally { fixture.close(); }
});

test('STREAM actor bind: admitted request with reply withheld exhausts as DeadlineExceeded', async () => {
  let ingress = 0;
  const fixture = bindFixture(async attempt => {
    ingress++;
    await delay(attempt.timeoutMs);
    throw new ZLinkBackendResultError('request', RequestResult.TimedOut);
  });
  try {
    await assert.rejects(fixture.bind(80), error => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      return true;
    });
    assert.equal(ingress, 1);
    assert.equal(fixture.attempts.length, 1);
    assert.equal(fixture.runtime.allSessionBindings().length, 0);
    fixture.checkAttempts();
  } finally { fixture.close(); }
});

test('STREAM actor bind: handover NOT_CONNECTED replays stable identity within the original deadline', async () => {
  let executions = 0;
  let terminal;
  const fixture = bindFixture(async (attempt, number) => {
    if (terminal === undefined) {
      executions++;
      terminal = successfulBind(attempt);
    }
    if (number === 1) {
      await delay(20);
      throw new ZLinkBackendResultError('request', RequestResult.NotConnected);
    }
    return terminal;
  });
  try {
    const start = performance.now();
    await fixture.bind(200);
    assert(performance.now() - start < 200);
    assert.equal(fixture.attempts.length, 2);
    assert.equal(executions, 1);
    assert.deepEqual(fixture.runtime.sessionBindings('session-rid').map(b => b.bindingGeneration), [11n]);
    await delay(25);
    assert.equal(fixture.attempts.length, 2, 'a received terminal stops replay');
    fixture.checkAttempts();
  } finally { fixture.close(); }
});

test('STREAM actor bind retains admission history after handover followed by route absence', async () => {
  const fixture = bindFixture(async (_attempt, number) => {
    throw number === 1
      ? new ZLinkBackendResultError('request', RequestResult.NotConnected)
      : new ZLinkBackendResultError('submit', SubmitResult.NotConnected);
  });
  try {
    await assert.rejects(fixture.bind(80), error => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      return true;
    });
    assert(fixture.attempts.length > 1);
    fixture.checkAttempts();
  } finally { fixture.close(); }
});

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
      await delay(25);
      assert.equal(fixture.attempts.length, 1);
      fixture.checkAttempts();
    } finally { fixture.close(); }
  }
});
