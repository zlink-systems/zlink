const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { RequestResult, SubmitResult, ZLinkBackendResultError } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { decodeActorJoin28 } = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');
const { encodeStatefulReply } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const { encodeApplicationPayload } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

function joinFixture(request, { canonical = true, entry = false } = {}) {
  const attempts = [];
  const runtime = new ServiceStatefulRuntime({
    setServiceIngress() {},
    topology: { peer: rid => rid === 'target-node' ? { descriptor: { lifecycleGeneration: 11n } } : undefined },
    async requestService(target, parts, timeoutMs) {
      const attempt = { target, parts, timeoutMs, at: performance.now(), record: decodeActorJoin28(parts) };
      attempts.push(attempt);
      return request(attempt, attempts.length);
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
  for (const failurePhase of ['submit', 'request']) for (const accepted of [true, false]) {
    test(`Actor Join replays original terminal after ${failurePhase} disconnect (canonical=${canonical}, entry=${entry}, accepted=${accepted})`, async t => {
      let now = 1000.25;
      t.mock.method(performance, 'now', () => now);
      t.mock.timers.enable({ apis: ['setTimeout'] });
      let terminal;
      let executions = 0;
      const fixture = joinFixture((attempt, number) => {
        if (failurePhase === 'submit' && number === 1) {
          throw new ZLinkBackendResultError('submit', SubmitResult.NotConnected);
        }
        if (terminal === undefined) {
          executions++;
          terminal = fixture.terminal(attempt, accepted);
        }
        if (number === 1) throw new ZLinkBackendResultError('request', RequestResult.NotConnected);
        return terminal;
      }, { canonical, entry });
      try {
        const pending = fixture.join(80);
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(fixture.attempts.length, 1);
        now += 20.25;
        t.mock.timers.tick(20.25);
        const result = await pending.promise;
        assert.equal(result.terminalResult, RequestResult.Ok);
        assert.equal(result.kindData.kind, 'actorJoinCompletion');
        assert.equal(result.kindData.joinResult, accepted ? 0 : 1);
        assert.equal(result.payload.payload.toString(), '{"original":true}');
        assert.equal(executions, 1);
        assert.equal(fixture.attempts.length, 2);
        for (const attempt of fixture.attempts) {
          assert.equal(attempt.target, 'target-node');
          assert.equal(attempt.record.correlation, pending.id, 'OperationId is unchanged');
          assert.equal(attempt.parts.length, 2, 'replay includes the application payload frame');
          assert.deepEqual(attempt.parts, fixture.attempts[0].parts);
          const drift = attempt.at + attempt.timeoutMs - 1080.25;
          assert(drift >= 0 && drift < 1, 'each attempt consumes the whole remaining original deadline');
        }
        assert.equal(fixture.runtime.canonicalActorJoinHandoffs.size, 0);
        now += 100;
        t.mock.timers.tick(100);
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(fixture.attempts.length, 2, 'a received terminal ends replay');
      } finally { fixture.runtime.close(); }
    });
  }
}

for (const admitted of [false, true]) {
  test(`Actor Join deadline has one owner and preserves admission history (admitted=${admitted})`, async t => {
    let now = 2000;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const fixture = joinFixture(() => {
      throw new ZLinkBackendResultError(admitted ? 'request' : 'submit',
        admitted ? RequestResult.NotConnected : SubmitResult.NotConnected);
    });
    try {
      const result = assert.rejects(fixture.join(80).promise, error => {
        assert.equal(error.kind, admitted
          ? framework.ZLinkFrameworkErrorKind.DeadlineExceeded
          : framework.ZLinkFrameworkErrorKind.Unavailable);
        return true;
      });
      await new Promise(resolve => setImmediate(resolve));
      now += 80;
      t.mock.timers.tick(80);
      await result;
      assert.equal(fixture.attempts.length, 1);
      assert.equal(fixture.runtime.canonicalActorJoinHandoffs.size, 0);
    } finally { fixture.runtime.close(); }
  });
}
