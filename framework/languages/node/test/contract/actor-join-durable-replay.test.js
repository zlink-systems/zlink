const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { RequestResult, SubmitResult, ZLinkBackendResultError } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { decodeActorJoin28 } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
const { encodeStatefulReply } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const { encodeApplicationPayload } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

function joinFixture(request, { canonical = true, entry = false, initiallyReady = true } = {}) {
  const attempts = [];
  let ready = initiallyReady;
  const routeObservers = new Set();
  const runtime = new ServiceStatefulRuntime({
    peerLifecycleGeneration: () => undefined,
    observePeerRoute: () => ({ generation: 1n, ready }),
    observePeerRouteChanged: listener => { routeObservers.add(listener); return () => routeObservers.delete(listener); },
    observePeerConnectionIntentRemoved() { return () => {}; },
    setServiceIngress() {},
    topology: { peer: rid => rid === 'target-node' ? { descriptor: { lifecycleGeneration: 11n } } : undefined },
    async requestService(target, parts, timeoutMs) {
      const attempt = { target, parts, timeoutMs, at: performance.now(), record: decodeActorJoin28(parts) };
      attempts.push(attempt);
      try { return await request(attempt, attempts.length); }
      catch (error) { ready = false; throw error; }
    }
  }, 'source-node', 7n);
  const actor = runtime.createActor('durable-actor');
  const spot = { spotId: entry ? 'target-node' : 'target-spot', generation: entry ? 11n : 3n };
  runtime.rememberSpotRoute({
    spot, targetNodeRid: 'target-node', targetNodeGeneration: 11n,
    authorityOwnerGeneration: 5n, ownerLeaseGeneration: 13n, storeVersion: 'spot-version'
  });
  const payload = { packetName: 'JoinInput', contentType: 'application/json', payload: Buffer.from('{"seat":2}') };
  return {
    runtime, attempts, spot,
    ready() { ready = true; for (const observer of routeObservers) observer('target-node'); },
    join(timeoutMs) {
      const args = entry ? [actor.ref, 'target-node', payload] : [actor.ref, 'target-node', spot, spot.generation, payload];
      if (canonical) args.push({ targetNodeGeneration: 7n, authorityOwnerGeneration: 1n, ownerLeaseGeneration: 7n },
        { phase: 'admission', transferId: 'local-only' });
      args.push(timeoutMs);
      const method = entry
        ? canonical ? 'joinActorEntrySpotCanonical' : 'joinActorEntrySpot'
        : canonical ? 'joinActorCanonical' : 'joinActor';
      return runtime[method](...args);
    },
    terminal(attempt, accepted = true) {
      return [encodeStatefulReply(attempt.record.correlation, RequestResult.Ok, 0, {
        kind: 'actorJoin', joinResult: accepted ? 0 : 1, spot, membershipEpoch: 2n
      }), encodeApplicationPayload({ packetName: 'JoinReply', contentType: 'application/json', payload: Buffer.from('{"original":true}') })];
    }
  };
}

for (const canonical of [true, false]) for (const entry of [true, false]) {
  for (const accepted of [true, false]) {
    test(`Actor Join retains original reply across ready-route changes (canonical=${canonical}, entry=${entry}, accepted=${accepted})`, async t => {
      let now = 1000.25;
      t.mock.method(performance, 'now', () => now);
      t.mock.timers.enable({ apis: ['setTimeout'] });
      let finish;
      const fixture = joinFixture(attempt => new Promise(resolve => {
        finish = () => resolve(fixture.terminal(attempt, accepted));
      }), { canonical, entry, initiallyReady: false });
      try {
        const pending = fixture.join(80);
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(fixture.attempts.length, 0);
        now += 20.25;
        fixture.ready();
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(fixture.attempts.length, 1);
        fixture.ready();
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(fixture.attempts.length, 1);
        finish();
        const result = await pending.promise;
        assert.equal(result.terminalResult, RequestResult.Ok);
        assert.equal(result.kindData.kind, 'actorJoinCompletion');
        assert.equal(result.kindData.joinResult, accepted ? 0 : 1);
        assert.equal(result.payload.payload.toString(), '{"original":true}');
        const attempt = fixture.attempts[0];
        assert.equal(attempt.target, 'target-node');
        assert.equal(attempt.record.correlation, pending.id);
        assert.equal(attempt.parts.length, 2);
        assert.equal(attempt.timeoutMs, 60);
        assert.equal(pending.deadlineMs, 1080.25);
        assert.equal(fixture.runtime[inspectRuntime]().canonicalActorJoinHandoffs, 0);
        now += 100;
        fixture.ready();
        t.mock.timers.tick(100);
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(fixture.attempts.length, 1);
      } finally { fixture.runtime.close(); }
    });
  }
  for (const phase of ['submit', 'request']) {
    test(`Actor Join returns first ${phase} failure once (canonical=${canonical}, entry=${entry})`, async () => {
      const failure = new ZLinkBackendResultError(phase,
        phase === 'submit' ? SubmitResult.NotConnected : RequestResult.NotConnected);
      const fixture = joinFixture(() => { throw failure; }, { canonical, entry });
      try {
        await assert.rejects(fixture.join(80).promise, error => error === failure);
        fixture.ready();
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(fixture.attempts.length, 1);
        assert.equal(fixture.runtime[inspectRuntime]().canonicalActorJoinHandoffs, 0);
      } finally { fixture.runtime.close(); }
    });
  }
}

for (const started of [false, true]) {
  test(`Actor Join registry owns the original deadline (started=${started})`, async t => {
    let now = 2000;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    let finish;
    const fixture = joinFixture(attempt => new Promise(resolve => {
      finish = () => resolve(fixture.terminal(attempt));
    }), { initiallyReady: started });
    try {
      const pending = fixture.join(80);
      const result = assert.rejects(pending.promise, error => {
        assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
        return true;
      });
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.runtime.nextMaintenanceDeadlineMs(), 2080);
      now += 80;
      assert.equal(fixture.runtime.expireOperations(now, Number.POSITIVE_INFINITY), 1);
      await result;
      finish?.();
      fixture.ready();
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(fixture.attempts.length, started ? 1 : 0);
      assert.equal(fixture.runtime[inspectRuntime]().canonicalActorJoinHandoffs, 0);
    } finally { fixture.runtime.close(); }
  });
}
