const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const SHUTDOWN_DEADLINE_MS = 1000;

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
