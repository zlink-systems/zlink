'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkChannelReceiveLoop
} = require('../../packages/framework/dist/runtime/channels/channel-receive-loops');
const {
  ZLinkRoutePacketDispatcher
} = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const {
  ZLinkDispatchErrorReporter
} = require('../../packages/framework/dist/runtime/channels/dispatch-error-reporter');
const {
  encodeChannelEnvelopeParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  ZLinkSpotActorPacketDrain
} = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-drain');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((done, fail) => {
    resolve = done;
    reject = fail;
  });
  return { promise, resolve, reject };
}

async function until(check, label) {
  for (let i = 0; i < 100; i += 1) {
    if (check()) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.fail(`${label} was not observed`);
}

test('remote channel receipt keeps the injected time while waiting in the dispatch queue', async () => {
  const acceptedAt = new Date('2026-11-02T01:23:45.678Z');
  let now = acceptedAt;
  let clockCalls = 0;
  let queuedOperation;
  const dispatchDone = deferred();
  let recordRead = false;
  const record = {
    parts: [],
    routingId: 'remote-node',
    replyToken: null,
    close() {}
  };
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  let ready = true;
  const loop = new ZLinkChannelReceiveLoop(
    'received-at',
    { recv() {
      if (recordRead) return undefined;
      recordRead = true;
      return record;
    }, reply() { throw new Error('unexpected reply'); } },
    {
      flowEnabled() { return false; },
      async dispatch(_received, _router, _signal, _header, receivedAt) {
        assert.equal(receivedAt.getTime(), acceptedAt.getTime());
      }
    },
    undefined,
    undefined,
    {
      wait() { return ready; },
      markDrained() { ready = false; },
      async waitForReadable(signal) {
        if (signal.aborted) return false;
        await new Promise(resolve => signal.addEventListener('abort', resolve, { once: true }));
        return false;
      },
      dispose() {}
    },
    queue,
    undefined,
    undefined,
    {
      submitPreAdmitted(operation) {
        queuedOperation = async () => {
          try {
            dispatchDone.resolve(await operation());
          } catch (error) {
            dispatchDone.reject(error);
            throw error;
          }
        };
        return dispatchDone.promise;
      }
    },
    () => {
      clockCalls += 1;
      return new Date(now);
    }
  );

  const running = loop.run();
  try {
    await until(() => queuedOperation !== undefined, 'queued remote dispatch');
    now = new Date(acceptedAt.getTime() + 10_000);
    const stopping = loop.stop();
    await queuedOperation();
    await stopping;
    await running;
    assert.equal(clockCalls, 1);
  } finally {
    await loop.stop();
  }
});

test('local route submission keeps its injected acceptance time through handler work', async () => {
  const acceptedAt = new Date('2026-11-02T02:34:56.789Z');
  let now = acceptedAt;
  let clockCalls = 0;
  const handlerStarted = deferred();
  const finishHandler = deferred();
  const dispatcher = new ZLinkRoutePacketDispatcher({
    routerChannelId: 'mesh-a',
    receivedAtClock() {
      clockCalls += 1;
      return new Date(now);
    },
    dispatchErrors: new ZLinkDispatchErrorReporter(),
    handlers: [{
      kind: 'send',
      packetName: 'Notice',
      handler: {
        async handle(_payload, context) {
          handlerStarted.resolve(context.receivedAt);
          await finishHandler.promise;
        }
      }
    }]
  });
  const parts = encodeChannelEnvelopeParts(3, 'mesh-a', 'Notice', { value: 'local' },
    undefined, undefined, undefined, undefined, false).map(ZLinkBufferMessage.from);

  const dispatch = dispatcher.dispatchLocalCommand(parts, 'node-a');
  const receivedAt = await handlerStarted.promise;
  now = new Date(acceptedAt.getTime() + 10_000);
  finishHandler.resolve();
  await dispatch;

  assert.equal(receivedAt.getTime(), acceptedAt.getTime());
  assert.equal(clockCalls, 1);
  parts.forEach(part => part.close());
});

test('remote Actor packet drain carries its injected receive time to dispatch', async () => {
  const acceptedAt = new Date('2026-11-02T03:45:12.345Z');
  let now = acceptedAt;
  let clockCalls = 0;
  const handlerStarted = deferred();
  const finishHandler = deferred();
  const message = { data: () => Buffer.alloc(0), close() {} };
  const drain = new ZLinkSpotActorPacketDrain({
    receivedAtClock() {
      clockCalls += 1;
      return new Date(now);
    },
    waitIdle: async () => {},
    actorPacketHandler: async delivery => {
      handlerStarted.resolve(delivery.receivedAt);
      await finishHandler.promise;
    }
  });
  let claimed = false;
  const draining = drain.drain({
    recvActorPart() {
      if (claimed) return null;
      claimed = true;
      return {
        info: { actor: { nodeRid: 'node-b', actorId: 'actor-a', generation: 1n } },
        message,
        more: false
      };
    }
  });

  const receivedAt = await handlerStarted.promise;
  now = new Date(acceptedAt.getTime() + 10_000);
  finishHandler.resolve();
  await draining;
  drain.dispose();

  assert.equal(receivedAt.getTime(), acceptedAt.getTime());
  assert.equal(clockCalls, 1);
});
