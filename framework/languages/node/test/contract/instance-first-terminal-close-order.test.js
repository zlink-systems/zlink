const assert = require('node:assert/strict');
const test = require('node:test');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');

// Spot messaging §4 step 12 with §7 step 1: a Close waits for the first terminal
// record of an activated message whose handler turn started before it. A message
// still queued behind that Close has not started, so the Close must not wait for
// it; otherwise the Close and the queued message wait for each other.
function runtime() {
  return new ServiceStatefulRuntime({
    topology: { peer: () => ({ descriptor: { lifecycleGeneration: 5n } }) },
    observePeerConnectionIntentRemoved: () => () => {},
    setServiceIngress() {},
    replyService() {}
  }, 'target', 7n);
}

const target = { targetSpotId: 'room-1', stableType: 'Room' };
const route = {
  targetSpotId: 'room-1',
  targetNodeRid: 'target',
  targetNodeGeneration: 7n,
  objectGeneration: 1n,
  authorityOwnerGeneration: 1n,
  ownerId: 'owner',
  leaseGeneration: 1n,
  storeVersion: 'v1'
};

async function settled(promise) {
  let done = false;
  void promise.then(() => { done = true; });
  await new Promise((resolve) => setImmediate(resolve));
  return done;
}

test('Close does not wait for the first terminal of an activated message that has not started', async () => {
  const stateful = runtime();
  try {
    const queued = stateful.activationTerminalCompletion(target, route);
    assert.equal(await settled(stateful.waitForInstanceApplicationQuiescence('room-1')), true);
    await queued.onTerminalCompletion();
    assert.equal(await settled(stateful.waitForInstanceApplicationQuiescence('room-1')), true);
  } finally {
    stateful.close();
  }
});

test('Close waits for the first terminal record of a started activated message', async () => {
  const stateful = runtime();
  try {
    const started = stateful.activationTerminalCompletion(target, route);
    started.onHandlerTurnStarted();
    const wait = stateful.waitForInstanceApplicationQuiescence('room-1');
    assert.equal(await settled(wait), false);
    await started.onTerminalCompletion();
    assert.equal(await settled(wait), true);
  } finally {
    stateful.close();
  }
});
