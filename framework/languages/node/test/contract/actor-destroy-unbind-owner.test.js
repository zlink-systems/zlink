const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkActorRuntimeOptionsFactory
} = require('../../packages/framework/dist/runtime/host/actor-runtime-options-factory');

test('destroy cleanup propagates a failed session unbind to the destroy terminal', async () => {
  const failure = new Error('unbound control on a closed connection');
  const reported = [];
  const used = {
    registration: { messageSerializers: new Map(), requestTimeoutMs: 1000 },
    invalidateActorRoute() {},
    rememberDestroyedActorRef() {},
    streamBindingRuntime: {
      async unbindActor() {
        throw failure;
      }
    },
    reportPostCommitError: (error) => reported.push(error)
  };
  const factoryOptions = new Proxy(used, {
    get: (target, key) => (key in target ? target[key] : () => undefined)
  });
  const options = new ZLinkActorRuntimeOptionsFactory(factoryOptions).createActorManagerOptions();
  await assert.rejects(options.actorDestroyedCleanup('actor-a'), (error) => error === failure);
  assert.deepEqual(reported, []);
});
