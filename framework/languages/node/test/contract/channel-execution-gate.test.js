const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkChannelReceiveLoop,
  ZLinkRouteReceiveLoop,
  ZLinkSubscriberReceiveLoop,
  createChannelApplicationDispatchQueue
} = require('../../packages/framework/dist/runtime/channels/channel-receive-loops');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');

function deferred() {
  let resolve;
  const promise = new Promise((complete) => { resolve = complete; });
  return { promise, resolve };
}

function record(kind) {
  return {
    kind,
    parts: [],
    routingId: kind,
    replyToken: kind === 'send' ? null : kind,
    closes: 0,
    terminals: 0,
    complete() { this.terminals += 1; },
    close() { this.closes += 1; }
  };
}

function receiveLoop(channelName, records, dispatch, queue, readablePoller, reportError) {
  const stopped = deferred();
  const loop = new ZLinkChannelReceiveLoop(
    channelName,
    { recv() { return records.shift(); }, reply() { throw new Error('unexpected reply'); } },
    { flowEnabled() { return false; }, dispatch },
    undefined,
    undefined,
    readablePoller ?? {
      wait() { return records.length > 0; },
      waitForReadable(signal) {
        if (signal.aborted) return Promise.resolve(false);
        signal.addEventListener('abort', () => stopped.resolve(), { once: true });
        return stopped.promise.then(() => false);
      },
      markDrained() {},
      dispose() {}
    },
    queue,
    reportError
  );
  return { loop, running: loop.run() };
}

function poller(records) {
  const stopped = deferred();
  return {
    wait() { return records.length > 0; },
    waitForReadable(signal) {
      if (signal.aborted) return Promise.resolve(false);
      signal.addEventListener('abort', () => stopped.resolve(), { once: true });
      return stopped.promise.then(() => false);
    },
    markDrained() {},
    dispose() {}
  };
}

async function until(check, label) {
  for (let i = 0; i < 100; i += 1) {
    if (check()) return;
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  assert.fail(`${label} was not observed`);
}

function notifyingPoller(records) {
  let notify;
  return {
    wait() { return records.length > 0; },
    waitForReadable(signal) {
      if (signal.aborted) return Promise.resolve(false);
      if (records.length > 0) return Promise.resolve(true);
      return new Promise((resolve) => {
        notify = resolve;
        signal.addEventListener('abort', () => resolve(false), { once: true });
      });
    },
    wake() { notify?.(true); },
    markDrained() {},
    dispose() {}
  };
}

for (const path of ['client-server', 'route']) {
  for (const firstKind of ['send', 'request']) {
    for (const secondKind of ['send', 'request']) {
      for (const batch of ['same', 'next', 'failure']) {
        test(`${path} ${firstKind}->${secondKind} ${batch} batch starts FIFO without awaiting terminal`, async () => {
          const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
          const release = deferred();
          const first = record(firstKind);
          const second = record(secondKind);
          const records = batch === 'next' ? [first] : [first, second];
          const readable = notifyingPoller(records);
          const started = [];
          const completed = [];
          const errors = [];
          const failure = new Error('first handler rejected');
          const disposed = [];
          class Dependency {
            dispose() { disposed.push(this.id); }
          }
          const {
            runInHandlerInstanceScope
          } = require('../../packages/framework/dist/runtime/handlers/handler-instance-scope');
          const dispatch = (received) =>
            runInHandlerInstanceScope(
              {
                create(type) {
                  return new type();
                }
              },
              undefined,
              async (scope) => {
                const id = received === first ? 1 : 2;
                started.push(id);
                const dependency = await scope.resolve(Dependency);
                dependency.id = id;
                if (id === 1) {
                  if (batch === 'next') {
                    records.push(second);
                    readable.wake();
                  }
                  await release.promise;
                  assert.equal(first.closes, 0, 'payload and reply token survive suspension');
                  assert.deepEqual(disposed, [2]);
                  if (batch === 'failure') {
                    completed.push(id);
                    throw failure;
                  }
                }
                if (received.replyToken !== null) received.complete();
                completed.push(id);
              }
            );
          let loop;
          let running;
          if (path === 'client-server') {
            ({ loop, running } = receiveLoop(
              'orders',
              records,
              dispatch,
              queue,
              readable,
              (error) => errors.push(error)
            ));
          } else {
            loop = new ZLinkRouteReceiveLoop(
              {
                recv() { return records.shift(); },
                reply() { throw new Error('unexpected reply'); }
              },
              {
                flowEnabled() { return false; },
                dispatchInfrastructure() { return false; },
                dispatch
              },
              readable,
              queue,
              undefined,
              (error) => errors.push(error)
            );
            running = loop.run();
          }
          try {
            await until(() => second.closes === 1, 'second terminal before first release');
            assert.deepEqual(started, [1, 2]);
            assert.deepEqual(completed, [2]);
            assert.equal(first.closes, 0);
            assert.equal(first.terminals, 0);
            assert.equal(second.terminals, secondKind === 'request' ? 1 : 0);
            let stopped = false;
            const stopping = loop.stop().then(() => { stopped = true; });
            await new Promise((resolve) => setImmediate(resolve));
            assert.equal(stopped, false, 'stop waits for terminal after queue drain');
            release.resolve();
            await stopping;
            assert.deepEqual(completed, [2, 1]);
            assert.deepEqual([first.closes, second.closes], [1, 1]);
            assert.equal(first.terminals, firstKind === 'request' && batch !== 'failure' ? 1 : 0);
            assert.deepEqual(disposed, [2, 1]);
            assert.equal(queue.snapshot().permitsInUse, 0n);
            assert.deepEqual(errors, batch === 'failure' ? [failure] : []);
          } finally {
            release.resolve();
            await loop.stop();
            await running;
          }
        });
      }
    }
  }
}

function subscriberRecord(id) {
  const received = { id, topic: 'orders', closes: 0 };
  received.parts = [
    {
      data() {
        return Buffer.from(String(id));
      },
      close() {
        received.closes += 1;
      }
    }
  ];
  return received;
}

function subscriberLoop(records, queue, dispatch, reportError, shared) {
  const readable = notifyingPoller(records);
  const loop = new ZLinkSubscriberReceiveLoop(
    {
      createTopicMessage() { return { topic: '', parts: [], close() {} }; },
      createReadablePoller() { return readable; }
    },
    {
      subscribe(message) {
        const next = records.shift();
        if (next === undefined) return false;
        Object.assign(message, next);
        return true;
      }
    },
    { dispatch },
    queue,
    undefined,
    undefined,
    reportError,
    shared
  );
  return { loop, readable };
}

for (const batch of ['same', 'next', 'failure']) {
  test(`subscriber starts FIFO without awaiting the asynchronous terminal: ${batch} batch`, async () => {
    const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
    const first = subscriberRecord(1);
    const second = subscriberRecord(2);
    const records = batch === 'next' ? [first] : [first, second];
    const release = deferred();
    const started = [];
    const completed = [];
    const errors = [];
    const failure = new Error('first subscriber handler rejected');
    const { loop, readable } = subscriberLoop(
      records,
      queue,
      async (received) => {
        started.push(received.id);
        if (received.id === 1) {
          if (batch === 'next') {
            records.push(second);
            readable.wake();
          }
          await release.promise;
          assert.equal(first.closes, 0, 'first payload survives suspension');
        }
        completed.push(received.id);
        if (received.id === 1 && batch === 'failure') throw failure;
      },
      (error) => errors.push(error)
    );
    const running = loop.run();
    try {
      await until(() => second.closes === 1, 'second subscriber terminal before first release');
      assert.deepEqual(started, [1, 2]);
      assert.deepEqual(completed, [2]);
      assert.equal(first.closes, 0);
      let stopped = false;
      const stopping = loop.stop().then(() => { stopped = true; });
      await new Promise((resolve) => setImmediate(resolve));
      assert.equal(stopped, false, 'stop waits for the first subscriber terminal');
      release.resolve();
      await stopping;
      assert.deepEqual(completed, [2, 1]);
      assert.deepEqual([first.closes, second.closes], [1, 1]);
      assert.equal(queue.snapshot().permitsInUse, 0n);
      assert.deepEqual(errors, batch === 'failure' ? [failure] : []);
    } finally {
      release.resolve();
      await loop.stop();
      await running;
    }
  });
}

test('subscriber connections of one ChannelName start FIFO without holding an application gate', async () => {
  const shared = createChannelApplicationDispatchQueue();
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const first = subscriberRecord(1);
  const second = subscriberRecord(2);
  const entered = deferred();
  const release = deferred();
  const started = [];
  const completed = [];
  const errors = [];
  const dispatch = async (received) => {
    started.push(received.id);
    if (received.id === 1) {
      entered.resolve();
      await release.promise;
      assert.equal(first.closes, 0);
    }
    completed.push(received.id);
  };
  const firstLoop = subscriberLoop(
    [first],
    queue,
    dispatch,
    (error) => errors.push(error),
    shared
  ).loop;
  const secondLoop = subscriberLoop(
    [second],
    queue,
    dispatch,
    (error) => errors.push(error),
    shared
  ).loop;
  const firstRunning = firstLoop.run();
  let secondRunning;
  try {
    await entered.promise;
    secondRunning = secondLoop.run();
    await until(() => second.closes === 1, 'second connection terminal before first release');
    assert.deepEqual(started, [1, 2]);
    assert.deepEqual(completed, [2]);
    assert.equal(first.closes, 0);
    let stopped = false;
    const stopping = firstLoop.stop().then(() => { stopped = true; });
    await secondLoop.stop();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(stopped, false, 'first connection stop waits for its terminal');
    release.resolve();
    await stopping;
    assert.deepEqual(completed, [2, 1]);
    assert.deepEqual([first.closes, second.closes], [1, 1]);
    assert.equal(queue.snapshot().permitsInUse, 0n);
    assert.deepEqual(errors, []);
  } finally {
    release.resolve();
    await Promise.all([firstLoop.stop(), secondLoop.stop()]);
    await Promise.all([firstRunning, secondRunning]);
  }
});
