const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');

function fixture({ entry, lifecycleFailure, completionFailure, sourceLeaveFailure, throwUndefined } = {}) {
  const events = [];
  const completions = [];
  const notificationErrors = [];
  const runner = new framework.ZLinkRuntimeTaskRunner({
    reportRuntimeTaskException(taskName, error) { notificationErrors.push({ taskName, error }); }
  }, new AbortController().signal);
  const actorRef = { actorId: 'actor-a', nodeRid: 'target', objectGeneration: 1n };
  const actor = { context: { actorId: 'actor-a' } };
  const completion = {
    status: 'accepted',
    operationId: { high: 1n, low: 2n },
    actor: actorRef,
    rawReply: Buffer.alloc(0)
  };
  const joined = async () => {
    events.push('joined');
    if (throwUndefined || lifecycleFailure !== undefined) throw lifecycleFailure;
  };
  const activation = {
    spot: { onJoinedActor: joined },
    serial: { execute: operation => operation() },
    executeActor: (_actorId, operation) => operation()
  };
  const manager = {
    formalRemoteActorAdmissions: {
      get: () => ({
        state: 'admitted',
        admission: { actorId: 'actor-a', spotId: entry ? 'entry' : 'room', actorRef: { nodeRid: 'source' } },
        result: { accepted: true, deferredJoinCompletion: completion }
      }),
      markCommitted: () => events.push('opened')
    },
    activations: { resolve: () => activation },
    options: {
      entryNodeRid: 'entry',
      detachedTaskRunner: runner,
      dispatchEntryActorJoin: joined,
      actorTransferRuntime: {
        async deliverDeferredJoinCompletion(value, target, currentRef, submitMailbox) {
          assert.equal(target, actor);
          assert.equal(currentRef, actorRef);
          await submitMailbox(async () => {
            completions.push(value);
            events.push(`completion:${value.status}`);
            if (completionFailure !== undefined) throw completionFailure;
          });
        }
      }
    }
  };
  return {
    events,
    completions,
    notificationErrors,
    completion,
    run: () => DefaultZLinkSpotManager.prototype.finalizeActorJoinRelocation.call(
      manager, 'mesh', 'relocation-a', actor, actorRef, async () => {
        events.push('source-leave');
        if (sourceLeaveFailure !== undefined) throw sourceLeaveFailure;
      }
    )
  };
}

for (const entry of [false, true]) {
  test(`canonical ${entry ? 'Entry' : 'User'} target lifecycle failure reaches Failed completion and preserves the barrier`, async () => {
    const failure = new framework.ZLinkFrameworkException(framework.ZLinkFrameworkErrorKind.DataLost, 'restore failed');
    const f = fixture({ entry, lifecycleFailure: failure });
    await assert.rejects(f.run(), error => error === failure);
    assert.deepEqual(f.events, ['joined', 'completion:failed']);
    assert.equal(f.completions.length, 1);
    assert.equal(f.completions[0].operationId, f.completion.operationId);
    assert.equal(f.completions[0].kind, framework.ZLinkFrameworkErrorKind.DataLost);
  });

  test(`canonical ${entry ? 'Entry' : 'User'} completion failure does not submit another completion`, async () => {
    const failure = new Error('application completion failed');
    const f = fixture({ entry, completionFailure: failure });
    await assert.rejects(f.run(), error => error === failure);
    assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted']);
    assert.equal(f.completions.length, 1);
  });

  test(`canonical ${entry ? 'Entry' : 'User'} successful Join completes before dispatch opens`, async () => {
    const f = fixture({ entry });
    assert.equal(await f.run(), true);
    assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted', 'opened']);
  });
}

test('canonical target unexpected lifecycle failure becomes InternalFailure', async () => {
  const failure = new Error('joined failed');
  const f = fixture({ lifecycleFailure: failure });
  await assert.rejects(f.run(), error => error === failure);
  assert.equal(f.completions[0].kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
});

test('canonical source leave submission failure is reported without blocking Accepted completion', async () => {
  const failure = new Error('source notification submission failed');
  const f = fixture({ sourceLeaveFailure: failure });
  assert.equal(await f.run(), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted', 'opened']);
  assert.equal(f.completions.length, 1);
  assert.equal(f.notificationErrors.length, 1);
  assert.equal(f.notificationErrors[0].error, failure);
});

test('canonical lifecycle rejection with undefined still delivers Failed and preserves the barrier', async () => {
  const f = fixture({ throwUndefined: true });
  await assert.rejects(f.run(), error => error === undefined);
  assert.deepEqual(f.events, ['joined', 'completion:failed']);
  assert.equal(f.completions[0].kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
});
