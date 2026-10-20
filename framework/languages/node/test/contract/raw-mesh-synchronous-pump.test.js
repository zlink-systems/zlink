const assert = require('node:assert/strict');
const { createHook } = require('node:async_hooks');
const test = require('node:test');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');

test('synchronous raw receive rotations do not create discarded runner Promises', async (t) => {
  let rotations = 0;
  let promises = 0;
  let readable;
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    setReadableHandler(handler) {
      readable = handler;
    },
    bind() {},
    localEndpoint() {
      return 'inproc://synchronous-pump';
    },
    routesSnapshot() {
      return [];
    },
    receive() {
      rotations += 1;
      return undefined;
    }
  };
  const backend = new ZLinkNodeRawMeshBackend(
    'synchronous-mesh',
    'local',
    {
      createHost() {
        return {
          createRouter() {
            return router;
          },
          shutdown() {},
          close() {}
        };
      }
    },
    new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2048n })
    )
  );
  backend.setBind('inproc://synchronous-pump');
  backend.start();
  await new Promise(setImmediate);
  rotations = 0;
  // The init hook observes actual V8 Promise creation, including async return values.
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE' && /at ZLinkNodeRawMeshBackend.runPump/.test(new Error().stack)) {
        promises += 1;
      }
    }
  });
  hook.enable();
  try {
    for (let index = 0; index < 1000; index += 1) readable(true, false);
  } finally {
    hook.disable();
    await backend.close();
  }
  t.diagnostic(JSON.stringify({ rotations, promises }));
  assert.equal(rotations, 1000);
  assert.equal(promises, 0);
});
