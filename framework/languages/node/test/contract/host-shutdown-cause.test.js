const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const SHUTDOWN_DEADLINE_MS = 1000;

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
