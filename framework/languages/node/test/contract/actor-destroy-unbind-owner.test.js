const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkActorRuntimeOptionsFactory } = require('../../packages/framework/dist/runtime/host/actor-runtime-options-factory');

// #4387: destroy cleanup starts the session unbind after the destroy commit. A failed unbound
// control (for example on a connection that already closed) must reach the post-commit error
// owner once instead of becoming an unhandled rejection.
test('destroy cleanup reports a failed session unbind to the post-commit error owner', async () => {
  const failure = new Error('unbound control on a closed connection');
  const reported = [];
  const unhandled = [];
  const onUnhandled = (reason) => unhandled.push(reason);
  process.on('unhandledRejection', onUnhandled);
  try {
    const used = {
      registration: { messageSerializers: new Map(), requestTimeoutMs: 1000 },
      invalidateActorRoute() {},
      rememberDestroyedActorRef() {},
      streamBindingRuntime: { unbindActor: async () => { throw failure; } },
      reportPostCommitError: (error) => reported.push(error)
    };
    // Only the destroy cleanup path is exercised; every other runtime option is absent.
    const factoryOptions = new Proxy(used, {
      get: (target, key) => (key in target ? target[key] : () => undefined)
    });
    const options = new ZLinkActorRuntimeOptionsFactory(factoryOptions).createActorManagerOptions();
    options.actorDestroyedCleanup('actor-a');
    await new Promise((resolve) => setImmediate(resolve));
    await new Promise((resolve) => setImmediate(resolve));
    assert.deepEqual(reported, [failure]);
    assert.deepEqual(unhandled, []);
  } finally {
    process.off('unhandledRejection', onUnhandled);
  }
});
