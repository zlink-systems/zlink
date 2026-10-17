const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const SHUTDOWN_DEADLINE_MS = 1000;

for (const aggregateWrapper of [false, true]) {
  for (const hasActualFailure of [false, true]) {
    test(`shutdown cleanup retains abort causes: aggregate=${aggregateWrapper} actual=${hasActualFailure}`, async () => {
      const { createAbortError } = require('../../packages/framework/dist/runtime/abort');
      const {
        stopRuntimeParts
      } = require('../../packages/framework/dist/runtime/host/runtime-shutdown');
      const error = aggregateWrapper
        ? new AggregateError([createAbortError()], 'cleanup aborted')
        : createAbortError();
      if (hasActualFailure)
        error.cause = new AggregateError([new Error('resource cleanup failed')]);
      const reported = [];
      const stopping = stopRuntimeParts({
        state: {
          listenerTasks: [],
          dispose() {},
          errorSink: {
            reportRuntimeTaskException(_operation, failure) {
              reported.push(failure);
            }
          }
        },
        locationSnapshot: {},
        streamRuntime: {
          async dispose() {
            throw error;
          }
        }
      });
      if (hasActualFailure) await assert.rejects(stopping, (failure) => failure === error);
      else await stopping;
      assert.deepEqual(reported, hasActualFailure ? [error] : []);
    });
  }
}

for (const nesting of ['deadline', 'nested', 'aggregate', 'pure']) {
  test(`host shutdown examines cleanup error causes: ${nesting}`, async () => {
    const deadline = (cause) =>
      framework.createInternalFrameworkException(
        framework.ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
        'cleanup deadline',
        cause
      );
    const actual = new Error('resource cleanup failed');
    const error =
      nesting === 'aggregate'
        ? new AggregateError([], 'cleanup deadline', { cause: actual })
        : deadline(
            nesting === 'pure' ? deadline() : nesting === 'nested' ? deadline(actual) : actual
          );
    const host = new framework.ZLinkFrameworkRuntimeHost({
      registration: framework.createFrameworkRegistration()
    });
    host.routeMeshCoordinator.shutdownHost = async () => ({ kind: 'drained' });
    host.stop = async () => {
      throw error;
    };
    const result = await host.shutdown({ deadlineMs: SHUTDOWN_DEADLINE_MS });
    assert.equal(result.outcome, framework.ZLinkFrameworkTerminationOutcome.ForceStopped);
    assert.equal(
      result.reason,
      nesting === 'pure'
        ? framework.ZLinkFrameworkTerminationReason.DeadlineExceeded
        : framework.ZLinkFrameworkTerminationReason.TeardownFailed
    );
  });
}

for (const scenario of [
  { name: 'callback failure before deadline', reason: 'teardown_failed', lateDeadline: true },
  {
    name: 'Store delete failure before deadline',
    reason: 'owner_cleanup_failed',
    lateDeadline: true
  },
  { name: 'callback timeout', reason: 'teardown_failed', lateDeadline: false },
  {
    name: 'unconfirmed Store result without exception',
    reason: 'owner_cleanup_failed',
    lateDeadline: true,
    noError: true
  },
  { name: 'deadline without failure', reason: 'deadline_exceeded', lateDeadline: true },
  { name: 'successful shutdown', lateDeadline: false }
]) {
  test(`host shutdown preserves classification: ${scenario.name}`, async () => {
    const { createDeadlineExceededError } = require('../../packages/framework/dist/runtime/abort');
    const host = new framework.ZLinkFrameworkRuntimeHost({
      registration: framework.createFrameworkRegistration()
    });
    host.routeMeshCoordinator.shutdownHost = async () =>
      scenario.reason === undefined
        ? { kind: 'drained' }
        : {
            kind: 'forceStopped',
            reason: scenario.reason,
            error: scenario.noError ? undefined : new Error(scenario.name)
          };
    host.stop = async () => {
      if (scenario.lateDeadline) throw createDeadlineExceededError();
    };
    const result = await host.shutdown({ deadlineMs: SHUTDOWN_DEADLINE_MS });
    assert.equal(
      result.outcome,
      scenario.reason === undefined
        ? framework.ZLinkFrameworkTerminationOutcome.Stopped
        : framework.ZLinkFrameworkTerminationOutcome.ForceStopped
    );
    assert.equal(
      result.reason,
      scenario.reason === undefined
        ? framework.ZLinkFrameworkTerminationReason.None
        : scenario.reason === 'deadline_exceeded'
          ? framework.ZLinkFrameworkTerminationReason.DeadlineExceeded
          : framework.ZLinkFrameworkTerminationReason.TeardownFailed
    );
  });
}

test('host queries and observation reuse one published status and sequence', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const first = host.status;
  assert.equal(host.status, first);
  const controller = new AbortController();
  const observer = host.observe(controller.signal)[Symbol.asyncIterator]();
  try {
    const event = await observer.next();
    assert.equal(event.value.status, first);
    assert.equal(host.status.sequence, first.sequence);
    assert.equal(host.status, first);
  } finally {
    controller.abort();
    await observer.return?.();
  }
});

test('host shutdown retains the original teardown exception for its owner', async () => {
  const failure = new Error('host resource teardown failed');
  const runtime = new framework.ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['play'],
    meshOptions: new Map([['play', {}]]),
    meshNode: () => undefined,
    admission: new framework.ZLinkRuntimeAdmissionGate(),
    publishRetiring: async () => {},
    rollbackRetiring: async () => {},
    publishDraining: async () => {},
    publishHostDraining: async () => {},
    drainResources: async () => {},
    cleanupHostResources: async () => {
      throw failure;
    },
    forceStopResources: async () => {}
  });
  const result = await runtime.shutdownHost(
    new Date(Date.now() + SHUTDOWN_DEADLINE_MS),
    new AbortController().signal
  );
  assert.equal(result.kind, 'forceStopped');
  assert.equal(result.reason, 'teardown_failed');
  assert.equal(result.error, failure);
});

test('host state publication before channel construction preserves the startup failure', async () => {
  const failure = new Error('channel adapter creation failed');
  const host = new framework.ZLinkFrameworkRuntimeHost(
    {
      registration: framework.createFrameworkRegistration()
    },
    {
      backendAdapterFactory: {
        createChannelAdapter() {
          throw failure;
        }
      }
    }
  );
  try {
    await assert.rejects(host.start(), (error) => error === failure);
    assert.equal(host.channelRuntime, undefined);
    assert.equal(host.isStarted, false);
    assert.doesNotThrow(() => host.setRuntimeState(framework.ZLinkFrameworkRuntimeState.Draining));
    assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Draining);
  } finally {
    await host.stop();
  }
});
