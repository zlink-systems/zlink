const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkManagedStream
} = require('../../packages/framework/dist/runtime/streams/managed-stream');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  wireReplyFailureException
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

function bindFixture(t, bindElapsedMs) {
  let now = 1000;
  t.mock.method(performance, 'now', () => now);
  const actor = { nodeRid: 'actor-owner', actorId: 'deadline-actor', generation: 7n };
  const binding = { actor, bindingGeneration: 11n };
  const bindTimeouts = [];
  const bindings = [];
  const service = {
    status: () => ({ state: 2 }),
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
      now += Math.min(bindElapsedMs, operation.timeoutMs);
      if (bindElapsedMs >= operation.timeoutMs) terminalResult = RequestResult.TimedOut;
      else bindings.push(binding);
      return {
        terminalResult,
        failureErrno: 0,
        parts: [],
        kindData: undefined
      };
    }
  };
  const stream = new ZLinkManagedStream({}, 'session-rid', undefined, service, completions);
  return {
    bindTimeouts,
    bindings,
    elapsedMs: () => now - 1000,
    bind: () =>
      stream.bindActor({ ...actor, objectGeneration: actor.generation, meshName: 'play' }, 80)
  };
}

test('STREAM Actor bind succeeds within its request deadline', async (t) => {
  const fixture = bindFixture(t, 10);
  await fixture.bind();
  assert.deepEqual(fixture.bindTimeouts, [80]);
  assert.equal(fixture.elapsedMs(), 10);
  assert.equal(fixture.bindings.length, 1);
});

test('STREAM Actor admission and completion consume only one deadline', async (t) => {
  const fixture = bindFixture(t, 80);
  const deadlineKind = wireReplyFailureException(RequestResult.TimedOut, 0, 'bind deadline').kind;
  await assert.rejects(fixture.bind(), (error) => {
    assert.equal(error.kind, deadlineKind);
    return true;
  });
  assert.deepEqual(fixture.bindTimeouts, [80]);
  assert.equal(fixture.elapsedMs(), 80);
  assert.equal(fixture.bindings.length, 0);
});
