const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkManagedStream
} = require('../../packages/framework/dist/runtime/streams/managed-stream');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  wireReplyFailureException
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

function bindFixture(t, lookupElapsedMs, bindElapsedMs) {
  let now = 1000;
  t.mock.method(performance, 'now', () => now);
  const actor = { nodeRid: 'actor-owner', actorId: 'deadline-actor', generation: 7n };
  const binding = { actor, bindingGeneration: 11n };
  const lookupTimeouts = [];
  const bindTimeouts = [];
  const bindings = [];
  const service = {
    status: () => ({ state: 2 }),
    lookupActor(_nodeRid, _actorId, timeoutMs) {
      lookupTimeouts.push(timeoutMs);
      return { kind: 'lookup' };
    },
    bindActor(_sessionRid, _actor, timeoutMs) {
      bindTimeouts.push(timeoutMs);
      return { kind: 'bind', timeoutMs };
    },
    bindings: () => bindings
  };
  const completions = {
    async submit(submit) {
      const operation = submit();
      let terminalResult = RequestResult.Ok;
      if (operation.kind === 'lookup') {
        now += lookupElapsedMs;
      } else {
        now += Math.min(bindElapsedMs, operation.timeoutMs);
        if (bindElapsedMs >= operation.timeoutMs) terminalResult = RequestResult.TimedOut;
        else bindings.push(binding);
      }
      return {
        terminalResult,
        failureErrno: 0,
        parts: [],
        kindData:
          operation.kind === 'lookup'
            ? { kind: 'actorLookupCompletion', location: { actor } }
            : undefined
      };
    }
  };
  const stream = new ZLinkManagedStream({}, 'session-rid', undefined, service, completions);
  return {
    lookupTimeouts,
    bindTimeouts,
    bindings,
    elapsedMs: () => now - 1000,
    bind: () =>
      stream.bindActor({ ...actor, objectGeneration: actor.generation, meshName: 'play' }, 80)
  };
}

test('STREAM Actor bind uses the time remaining after lookup and succeeds within the deadline', async (t) => {
  const fixture = bindFixture(t, 30, 10);
  await fixture.bind();
  assert.deepEqual(fixture.lookupTimeouts, [80]);
  assert.deepEqual(fixture.bindTimeouts, [50]);
  assert.equal(fixture.elapsedMs(), 40);
  assert.equal(fixture.bindings.length, 1);
});

test('STREAM Actor lookup and bind together consume only one deadline', async (t) => {
  const fixture = bindFixture(t, 30, 80);
  const deadlineKind = wireReplyFailureException(RequestResult.TimedOut, 0, 'bind deadline').kind;
  await assert.rejects(fixture.bind(), (error) => {
    assert.equal(error.kind, deadlineKind);
    return true;
  });
  assert.deepEqual(fixture.bindTimeouts, [50]);
  assert.equal(fixture.elapsedMs(), 80);
  assert.equal(fixture.bindings.length, 0);
});

for (const lookupElapsedMs of [80, 81]) {
  test(`STREAM Actor bind submits zero remaining budget after lookup takes ${lookupElapsedMs} ms`, async (t) => {
    const fixture = bindFixture(t, lookupElapsedMs, 0);
    const deadlineKind = wireReplyFailureException(RequestResult.TimedOut, 0, 'bind deadline').kind;
    await assert.rejects(fixture.bind(), (error) => {
      assert.equal(error.kind, deadlineKind);
      return true;
    });
    assert.deepEqual(fixture.lookupTimeouts, [80]);
    assert.deepEqual(fixture.bindTimeouts, [0]);
    assert.equal(fixture.elapsedMs(), lookupElapsedMs);
    assert.equal(fixture.bindings.length, 0);
  });
}
