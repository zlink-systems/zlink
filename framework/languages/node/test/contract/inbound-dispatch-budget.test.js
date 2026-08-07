const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const backend = require('../../packages/framework/dist/runtime/backend');

test('receive round-robin resumes with the next ready socket owner', () => {
  const coordinator = new framework.ZLinkReceiveRoundRobinCoordinator();
  const first = coordinator.register();
  const second = coordinator.register();
  const third = coordinator.register();
  coordinator.setReady(first, true);
  coordinator.setReady(second, true);
  coordinator.setReady(third, true);

  assert.equal(coordinator.tryAcquire(first), true);
  coordinator.release(first);
  assert.equal(coordinator.tryAcquire(first), false);
  assert.equal(coordinator.tryAcquire(second), true);
  coordinator.release(second);
  assert.equal(coordinator.tryAcquire(third), true);
  coordinator.release(third);

  coordinator.setReady(first, false);
  assert.equal(coordinator.tryAcquire(second), true);
  coordinator.unregister(second);
  assert.equal(coordinator.tryAcquire(third), true);
});

test('Channel and fanout HWM count only the application payload frame', async () => {
  const observed = [];
  const budget = {
    enqueue(bytes) { observed.push(bytes); },
    start() {},
    complete() {}
  };
  const header = message(4096);
  const payload = message(7);

  const channel = new framework.ZLinkChannelReceiveLoop(
    'api',
    {},
    { async dispatch() {} },
    undefined,
    undefined,
    budget
  );
  await channel.dispatchAndClose(received([header, payload]));

  const fanout = new framework.ZLinkSubscriberReceiveLoop(
    { createReadablePoller() { return { wait() {}, dispose() {} }; } },
    {},
    { async dispatch() {} },
    undefined,
    budget
  );
  await fanout.dispatchAndClose({ topic: 'orders', parts: [header, payload] });

  assert.deepEqual(observed, [7n, 7n]);
});

test('Channel request permit cancellation releases queued bytes and received ownership', async () => {
  const cancelled = new Error('cancelled');
  let queuedBytes = 0n;
  let closeCount = 0;
  let dispatched = false;
  const budget = rejectingBudget(cancelled, (bytes) => { queuedBytes = bytes; });
  const channel = new framework.ZLinkChannelReceiveLoop(
    'api',
    {},
    { async dispatch() { dispatched = true; } },
    undefined,
    undefined,
    budget
  );
  const packet = received([message(4), message(11)], 1n, () => { closeCount += 1; });

  await assert.rejects(channel.dispatchAndClose(packet), cancelled);

  assert.equal(queuedBytes, 0n);
  assert.equal(closeCount, 1);
  assert.equal(dispatched, false);
});

test('Mesh request permit rejection releases queued bytes and message parts', async () => {
  const rejected = new Error('permit rejected');
  let queuedBytes = 0n;
  let partCloseCount = 0;
  let readyHandler;
  let reportedError;
  const errorReported = new Promise((resolve) => { reportedError = resolve; });
  const part = message(13, () => { partCloseCount += 1; });
  let received = false;
  const claim = {
    recvBatch() {
      if (received) return { ok: false, records: [] };
      received = true;
      return {
        ok: true,
        records: [{ operationKind: 1, parts: [part] }]
      };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return { reset() {}, takeClaim() { return claim; }, close() {} };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    inboundDispatchBudget: rejectingBudget(
      rejected,
      (bytes) => { queuedBytes = bytes; }
    ),
    dispatch() {
      assert.fail('dispatch must not run without a completion permit');
    },
    reportError(error) {
      reportedError(error);
    }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Application);
    assert.equal(await errorReported, rejected);
    assert.equal(queuedBytes, 0n);
    assert.equal(partCloseCount, 1);
  } finally {
    await pump.dispose();
  }
});

test('Channel receive loop consumes detached dispatch rejections without unhandledRejection', async () => {
  const unhandled = [];
  const onUnhandled = (reason) => unhandled.push(reason);
  process.on('unhandledRejection', onUnhandled);
  let reported;
  const reportedError = new Promise((resolve) => { reported = resolve; });
  let receiveCount = 0;
  const failure = new Error('dispatch failed');
  const loop = new framework.ZLinkChannelReceiveLoop(
    'api',
    {
      recv() {
        if (receiveCount++ > 0) return undefined;
        return {
          parts: [],
          routingId: 'peer',
          requestSeq: null,
          close() {}
        };
      }
    },
    { async dispatch() { throw failure; } },
    undefined,
    undefined,
    undefined,
    { wait() { return true; }, dispose() {} },
    error => reported(error)
  );

  try {
    const running = loop.run();
    assert.equal(await reportedError, failure);
    await new Promise((resolve) => setImmediate(resolve));
    await loop.stop();
    await running;
    assert.deepEqual(unhandled, []);
  } finally {
    process.off('unhandledRejection', onUnhandled);
  }
});

test('Channel receive loop bounds detached dispatch concurrency for zero-byte messages', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  let active = 0;
  let maximumActive = 0;
  let receivedCount = 0;
  let completedCount = 0;
  const loop = new framework.ZLinkChannelReceiveLoop(
    'api',
    {
      recv() {
        if (receivedCount >= 1_025) return undefined;
        receivedCount += 1;
        return {
          parts: [],
          routingId: 'peer',
          requestSeq: null,
          close() {}
        };
      }
    },
    {
      async dispatch() {
        active += 1;
        maximumActive = Math.max(maximumActive, active);
        await gate;
        active -= 1;
        completedCount += 1;
      }
    },
    undefined,
    undefined,
    undefined,
    { wait() { return true; }, dispose() {} }
  );

  const running = loop.run();
  await waitFor(() => receivedCount === 1_024);
  assert.equal(maximumActive, 1_024);
  assert.equal(receivedCount, 1_024);

  release();
  await waitFor(() => completedCount === 1_025);
  await loop.stop();
  await running;
  assert.equal(maximumActive, 1_024);
});

test('multiplexed receive loops classify infrastructure while application HWM is paused', async () => {
  let budgetWaits = 0;
  const pausedBudget = {
    get receivePaused() { return true; },
    async waitUntilResumed() { budgetWaits += 1; await new Promise(() => {}); },
    enqueue() {},
    start() {},
    complete() {},
    cancelQueued() {}
  };

  let channelClosed = 0;
  let channelDispatches = 0;
  const channelQueue = [received([], null, () => { channelClosed += 1; })];
  const channel = new framework.ZLinkChannelReceiveLoop(
    'api',
    { recv() { return channelQueue.shift(); } },
    { async dispatch() { channelDispatches += 1; } },
    undefined,
    () => true,
    pausedBudget,
    { wait() { return channelQueue.length > 0; }, dispose() {} }
  );

  let subscriberClosed = 0;
  let subscriberDispatches = 0;
  const subscriberQueue = [{ topic: 'control', parts: [message(1, () => { subscriberClosed += 1; })] }];
  const subscriber = new framework.ZLinkSubscriberReceiveLoop(
    {
      createReadablePoller() {
        return { wait() { return subscriberQueue.length > 0; }, dispose() {} };
      },
      createTopicMessage() { return { topic: '', parts: [] }; }
    },
    {
      subscribe(target) {
        const next = subscriberQueue.shift();
        if (next === undefined) return false;
        target.topic = next.topic;
        target.parts = next.parts;
        return true;
      }
    },
    { async dispatch() { subscriberDispatches += 1; } },
    () => true,
    pausedBudget
  );

  let routeInfrastructure = 0;
  let routeDispatches = 0;
  const routeQueue = [received([])];
  const route = new framework.ZLinkRouteReceiveLoop(
    { recv() { return routeQueue.shift(); } },
    {
      dispatchInfrastructure() { routeInfrastructure += 1; return true; },
      async dispatch() { routeDispatches += 1; }
    },
    pausedBudget,
    { wait() { return routeQueue.length > 0; }, dispose() {} }
  );

  const running = [channel.run(), subscriber.run(), route.run()];
  await waitFor(() => channelClosed === 1 && subscriberClosed === 1 && routeInfrastructure === 1);
  await Promise.all([channel.stop(), subscriber.stop(), route.stop()]);
  await Promise.all(running);

  assert.equal(budgetWaits, 0);
  assert.equal(channelDispatches, 0);
  assert.equal(subscriberDispatches, 0);
  assert.equal(routeDispatches, 0);
});

test('paused application receive retains only one raw classification reservation', async () => {
  let paused = true;
  let resume;
  const resumed = new Promise((resolve) => { resume = resolve; });
  const budget = {
    get receivePaused() { return paused; },
    async waitUntilResumed() { await resumed; },
    enqueue() {},
    start() {},
    complete() {},
    cancelQueued() {}
  };
  let receivedCount = 0;
  let dispatched = 0;
  const loop = new framework.ZLinkChannelReceiveLoop(
    'api',
    {
      recv() {
        receivedCount += 1;
        return received([]);
      }
    },
    { async dispatch() { dispatched += 1; } },
    undefined,
    undefined,
    budget,
    { wait() { return receivedCount < 2; }, dispose() {} }
  );

  const running = loop.run();
  await waitFor(() => receivedCount === 1);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(receivedCount, 1);
  assert.equal(dispatched, 0);

  paused = false;
  resume();
  await waitFor(() => dispatched === 2);
  await loop.stop();
  await running;
});

test('host raw classification reservations stay at the fixed limit across receive loops', async () => {
  let paused = true;
  let resume;
  const resumed = new Promise((resolve) => { resume = resolve; });
  const budget = {
    get receivePaused() { return paused; },
    async waitUntilResumed() { await resumed; },
    enqueue() {},
    start() {},
    complete() {},
    cancelQueued() {}
  };
  const coordinator = new framework.ZLinkReceiveRoundRobinCoordinator();
  let receivedCount = 0;
  let dispatched = 0;
  let releaseHandlers;
  const handlersPending = new Promise((resolve) => { releaseHandlers = resolve; });
  const loops = Array.from(
    { length: framework.ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT + 1 },
    (_, index) => {
      let available = true;
      return new framework.ZLinkChannelReceiveLoop(
        `channel-${index}`,
        {
          recv() {
            if (!available) return undefined;
            available = false;
            receivedCount += 1;
            return received([]);
          }
        },
        {
          async dispatch() {
            dispatched += 1;
            await handlersPending;
          }
        },
        undefined,
        undefined,
        budget,
        { wait() { return available; }, dispose() {} },
        undefined,
        coordinator
      );
    }
  );

  const running = loops.map((loop) => loop.run());
  await waitFor(() => receivedCount === framework.ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(receivedCount, framework.ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT);

  paused = false;
  resume();
  await waitFor(() => dispatched === framework.ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(receivedCount, framework.ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT);

  releaseHandlers();
  await waitFor(() => dispatched === loops.length);
  await Promise.all(loops.map((loop) => loop.stop()));
  await Promise.all(running);
});

function message(size, onClose = () => {}) {
  return {
    data() { return Buffer.alloc(size); },
    size() { return size; },
    close: onClose
  };
}

function received(parts, requestSeq = null, onClose = () => {}) {
  return {
    parts,
    routingId: 'peer',
    requestSeq,
    close: onClose
  };
}

function rejectingBudget(error, observeQueued) {
  let queuedBytes = 0n;
  const publish = () => observeQueued(queuedBytes);
  return {
    get receivePaused() { return false; },
    enqueue(bytes) {
      queuedBytes += bytes;
      publish();
    },
    start(bytes) {
      queuedBytes -= bytes;
      publish();
    },
    cancelQueued(bytes) {
      queuedBytes -= bytes;
      publish();
    },
    complete() {},
    async acquireCompletionSend() {
      throw error;
    },
    onResume() {
      return () => {};
    }
  };
}

async function waitFor(predicate) {
  const deadline = Date.now() + 1_000;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error('condition timed out');
    await new Promise((resolve) => setImmediate(resolve));
  }
}
