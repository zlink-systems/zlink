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
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  ZLinkSpotActorPacketDrain
} = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-drain');
const {
  encodeChannelEnvelopeParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const serviceWire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { fixture } = require('./fixtures/raw-mesh-close');
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

test('mailbox receive time reaches send and request handlers after queued delay', async () => {
  const f = fixture();
  try {
    for (const kind of ['send', 'request']) {
      const envelopeKind = kind === 'send' ? 3 : 1;
      const channelParts = encodeChannelEnvelopeParts(
        envelopeKind,
        'orders',
        'Notice',
        { value: kind },
        undefined,
        undefined,
        undefined,
        kind === 'request' ? 'a'.repeat(32) : undefined,
        false
      );
      const frame = serviceWire.encodeMultipartApplicationPayload(
        channelParts.map(part => typeof part === 'string' ? Buffer.from(part) : part),
        SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
        SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
      );
      const mailboxRecord = {
        owner: 'channel:orders',
        domain: 'application',
        parts: [
          kind === 'send'
            ? serviceWire.encodeChannelSendHeader('orders')
            : serviceWire.encodeChannelRequestHeader(1n, 'orders'),
          frame
        ],
        sourceRoutingId: 'peer-a',
        ...(kind === 'request' ? { correlation: 1n } : {})
      };
      assert.equal(f.runtime.mailbox.tryEnqueue(mailboxRecord), true);
      const acceptedAt = mailboxRecord.receivedAt;
      assert.ok(acceptedAt instanceof Date);

      // Keep the admitted record in the mailbox while the clock advances.
      await new Promise(resolve => setTimeout(resolve, 10));
      const laterAt = new Date(acceptedAt.getTime() + 60_000);
      let dispatcherClockCalls = 0;
      let handlerReceivedAt;
      const dispatcher = new ZLinkRoutePacketDispatcher({
        routerChannelId: 'orders',
        receivedAtClock() {
          dispatcherClockCalls += 1;
          return laterAt;
        },
        dispatchErrors: new ZLinkDispatchErrorReporter(),
        handlers: [{
          kind,
          packetName: 'Notice',
          handler: {
            async handle(_payload, context) {
              handlerReceivedAt = context.receivedAt;
              return {};
            }
          }
        }]
      });

      const ready = f.backend.createReadyBatch(1);
      f.backend.drainReady(ReadyDomain.Application, ready);
      const claim = ready.takeClaim(0);
      let record;
      try {
        record = claim.recvBatch(f.backend.createReceiveBatch(1, 4)).records[0];
      } finally {
        claim.release();
        ready.close();
      }
      assert.ok(record);
      if (kind === 'request') record.reply = () => SubmitResult.Ok;
      try {
        await dispatcher.dispatchMesh(record);
        assert.equal(record.receivedAt.getTime(), acceptedAt.getTime());
        assert.equal(handlerReceivedAt.getTime(), acceptedAt.getTime());
        assert.equal(dispatcherClockCalls, 0);
      } finally {
        record.parts.forEach(part => part.close());
      }
    }
  } finally {
    await f.backend.close();
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
