const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkRuntimeDispatchErrorAction
} = require('../../packages/framework/dist/contracts/Dispatch/ZLinkDispatchOptions');
const traceRecords = [];
const observation = require('./fixtures/message-flow-observation.cjs')(
  require('../../packages/framework/dist/runtime/diagnostics/message-flow-delivery'),
  {
    onSpan(record) {
      traceRecords.push(record.attributes);
    }
  }
);
test.beforeEach(async () => observation.observed());
test.after(() => observation.close());
const {
  ZLinkDispatchErrorReporter
} = require('../../packages/framework/dist/runtime/channels/dispatch-error-reporter');
const {
  runWithFlow,
  currentOrCreateFlow
} = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');
const {
  ZLinkSpotSerialExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-executor');

function fixture({
  entry,
  lifecycleFailure,
  completionFailure,
  sourceLeaveFailure,
  throwUndefined
} = {}) {
  const events = [];
  const completions = [];
  const notificationErrors = [];
  const dispatchFailures = [];
  const rejectedPrefix = [];
  let prefixTerminal;
  const reporter = new ZLinkDispatchErrorReporter({
    diagnostics: { messageFlow: 'normal', sampleRate: 1, includeMessageSizes: false },
    liveMode: { mode: 'normal' }
  });
  const runner = new framework.ZLinkRuntimeTaskRunner(
    {
      reportRuntimeTaskException(taskName, error) {
        notificationErrors.push({ taskName, error });
      }
    },
    new AbortController().signal
  );
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
  const mailbox = new ZLinkSpotSerialExecutor(
    new framework.ZLinkSpotSerialTurnExecutor(false),
    framework.ZLinkUserSpotExecutionMode.SpotWide,
    entry ? 'entry' : 'room'
  );
  const activation = {
    spot: { onJoinedActor: joined },
    serial: { execute: (operation) => operation() }
  };
  const manager = {
    formalRemoteActorAdmissions: {
      get: () => ({
        state: 'admitted',
        admission: {
          actorId: 'actor-a',
          spotId: entry ? 'entry' : 'room',
          actorRef: { nodeRid: 'source' }
        },
        result: { accepted: true, deferredJoinCompletion: completion }
      }),
      markCommitted: () => events.push('opened')
    },
    activations: { resolve: () => activation },
    executeActorTurn: (_mesh, _spot, actorId, operation, onFailure) =>
      mailbox.executeActorLifecycle(actorId, operation, onFailure),
    options: {
      closeErrorSink: runner.errorSink,
      dispatchErrors: {
        captureEnabled: () => reporter.captureEnabled(),
        report: (failure) => {
          dispatchFailures.push(failure);
          reporter.report(failure);
        }
      },
      entryNodeRid: 'entry',
      detachedTaskRunner: runner,
      dispatchEntryActorJoin: (_mesh, _actor, _backlog, onCallbackFailure) =>
        joined().catch(onCallbackFailure),
      actorTransferRuntime: {
        async deliverDeferredJoinCompletion(value, target, currentRef, submitMailbox) {
          assert.equal(target, actor);
          assert.equal(currentRef, actorRef);
          await submitMailbox(async () => {
            completions.push(value);
            events.push(`completion:${value.status}`);
            if (completionFailure !== undefined) {
              prefixTerminal = mailbox.admitActorPacketPrefix(
                'actor-a',
                [
                  {
                    payloadBytes: 0,
                    drain: () => assert.fail('sealed Actor drained a captured packet'),
                    reject: (error) => rejectedPrefix.push(error)
                  }
                ],
                () => assert.fail('sealed Actor dispatched a captured packet')
              ).terminal;
              throw completionFailure;
            }
          });
        }
      }
    }
  };
  return {
    events,
    completions,
    notificationErrors,
    dispatchFailures,
    rejectedPrefix,
    async awaitPrefix() {
      await prefixTerminal;
    },
    completion,
    dispatchApplication: (operation) => mailbox.executeActor('actor-a', operation),
    run: () =>
      DefaultZLinkSpotManager.prototype.finalizeActorJoinRelocation.call(
        manager,
        'mesh',
        'relocation-a',
        actor,
        actorRef,
        async () => {
          events.push('source-leave');
          if (sourceLeaveFailure !== undefined) throw sourceLeaveFailure;
        }
      )
  };
}

for (const entry of [false, true]) {
  test(`canonical ${entry ? 'Entry' : 'User'} target lifecycle failure preserves Accepted completion and publication`, async () => {
    const failure = new framework.ZLinkFrameworkException(
      framework.ZLinkFrameworkErrorKind.DataLost,
      'restore failed'
    );
    const f = fixture({ entry, lifecycleFailure: failure });
    const flow = currentOrCreateFlow('Lifecycle');
    const previousRecords = traceRecords.length;
    assert.equal(await runWithFlow(flow, () => f.run()), true);
    await observation.observed();
    const recorded = traceRecords
      .slice(previousRecords)
      .find((record) => record.error_message === 'restore failed');
    assert.ok(recorded);
    assert.equal(recorded.flow_id, flow.flowId);
    assert.equal(recorded.error_type, failure.constructor.name);
    assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted', 'opened']);
    assert.equal(f.completions.length, 1);
    assert.equal(f.completions[0].operationId, f.completion.operationId);
    assert.equal(f.completions[0], f.completion);
    assert.deepEqual(f.notificationErrors, [{ taskName: 'OnJoinedActor actor-a', error: failure }]);
    assert.equal(f.dispatchFailures[0].action, ZLinkRuntimeDispatchErrorAction.Drop);
    assert.equal(f.dispatchFailures.length, 1);
    assert.equal(f.dispatchFailures[0].error, failure);
    assert.equal(f.dispatchFailures[0].error.message, 'restore failed');
  });

  test(`canonical ${entry ? 'Entry' : 'User'} completion failure is reported once and seals only application dispatch`, async () => {
    const failure = new Error('application completion failed');
    const f = fixture({ entry, completionFailure: failure });
    assert.equal(await f.run(), true);
    assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted', 'opened']);
    assert.equal(f.completions.length, 1);
    assert.deepEqual(f.notificationErrors, [
      { taskName: 'OnJoinCompleted actor-a', error: failure }
    ]);
    await assert.rejects(
      f.dispatchApplication(() => assert.fail('sealed Actor ran application work')),
      /Actor Join completion did not release application dispatch/
    );
    await f.awaitPrefix();
    assert.deepEqual(
      f.rejectedPrefix.map((error) => error.message),
      ['Actor Join completion did not release application dispatch.']
    );
    assert.equal(f.notificationErrors.length, 1);
  });

  test(`canonical ${entry ? 'Entry' : 'User'} successful Join completes before dispatch opens`, async () => {
    const f = fixture({ entry });
    assert.equal(await f.run(), true);
    assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted', 'opened']);
    assert.equal(await f.dispatchApplication(() => 'ran'), 'ran');
  });
}

test('canonical source leave submission failure is reported without blocking Accepted completion', async () => {
  const failure = new Error('source notification submission failed');
  const f = fixture({ sourceLeaveFailure: failure });
  assert.equal(await f.run(), true);
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted', 'opened']);
  assert.equal(f.completions.length, 1);
  assert.equal(f.notificationErrors.length, 1);
  assert.equal(f.notificationErrors[0].error, failure);
});

test('canonical lifecycle rejection with undefined is reported and preserves Accepted', async () => {
  const f = fixture({ throwUndefined: true });
  assert.equal(await f.run(), true);
  assert.deepEqual(f.events, ['joined', 'source-leave', 'completion:accepted', 'opened']);
  assert.equal(f.completions[0], f.completion);
  assert.deepEqual(f.notificationErrors, [{ taskName: 'OnJoinedActor actor-a', error: undefined }]);
});
