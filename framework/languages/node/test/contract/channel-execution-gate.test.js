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

function receiveLoop(channelName, records, dispatch, queue) {
  const stopped = deferred();
  const loop = new ZLinkChannelReceiveLoop(
    channelName,
    { recv() { return records.shift(); }, reply() { throw new Error('unexpected reply'); } },
    { flowEnabled() { return false; }, dispatch },
    undefined,
    undefined,
    {
      wait() { return records.length > 0; },
      waitForReadable(signal) {
        if (signal.aborted) return Promise.resolve(false);
        signal.addEventListener('abort', () => stopped.resolve(), { once: true });
        return stopped.promise.then(() => false);
      },
      markDrained() {},
      dispose() {}
    },
    queue
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

test('ChannelName gate retains an asynchronous request turn through send, request, and stop', async () => {
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const firstEntered = deferred();
  const releaseFirst = deferred();
  const otherCompleted = deferred();
  const sameRecords = [record('first'), record('send'), record('request')];
  const otherRecords = [record('other')];
  const allRecords = [...sameRecords, ...otherRecords];
  const started = [];
  let active = 0;
  let peak = 0;
  const same = receiveLoop('orders', sameRecords, async (received) => {
    started.push(received.kind);
    active += 1;
    peak = Math.max(peak, active);
    try {
      if (received.kind === 'first') {
        firstEntered.resolve();
        await releaseFirst.promise;
      }
      if (received.replyToken !== null) received.complete();
    } finally {
      active -= 1;
    }
  }, queue);
  const other = receiveLoop('support', otherRecords, async () => {
    otherCompleted.resolve();
  }, queue);
  let stopped = false;
  try {
    await firstEntered.promise;
    await until(() => sameRecords.length === 0, 'later send and request receive');
    await otherCompleted.promise;
    assert.deepEqual(started, ['first']);
    const stopping = same.loop.stop().then(() => { stopped = true; });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(stopped, false);
    releaseFirst.resolve();
    await stopping;
    assert.deepEqual(started, ['first', 'send', 'request']);
    assert.equal(peak, 1);
    assert.deepEqual(allRecords.map((received) => received.closes), [1, 1, 1, 1]);
    assert.deepEqual(allRecords.map((received) => received.terminals), [1, 0, 1, 0]);
  } finally {
    releaseFirst.resolve();
    await Promise.all([same.loop.stop(), other.loop.stop()]);
    await Promise.all([same.running, other.running]);
  }
});

for (const kind of ['route', 'subscriber']) {
  test(`${kind} ChannelName gate retains the asynchronous terminal`, async () => {
    const records = [record('first'), record('send'), record('request')];
    const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
    const entered = deferred();
    const release = deferred();
    const started = [];
    const dispatch = async (received) => {
      started.push(received.kind);
      if (received.kind === 'first') {
        entered.resolve();
        await release.promise;
      }
    };
    let loop;
    if (kind === 'route') {
      loop = new ZLinkRouteReceiveLoop(
        { recv() { return records.shift(); }, reply() { throw new Error('unexpected reply'); } },
        { flowEnabled() { return false; }, dispatchInfrastructure() { return false; }, dispatch },
        poller(records),
        queue
      );
    } else {
      const subscriber = {
        subscribe(message) {
          const next = records.shift();
          if (next === undefined) return false;
          message.kind = next.kind;
          return true;
        }
      };
      loop = new ZLinkSubscriberReceiveLoop(
        { createTopicMessage() { return { topic: '', parts: [] }; }, createReadablePoller() { return poller(records); } },
        subscriber,
        { dispatch },
        queue
      );
    }
    const running = loop.run();
    try {
      await entered.promise;
      await until(() => records.length === 0, `${kind} later records`);
      assert.deepEqual(started, ['first']);
      release.resolve();
      await until(() => started.length === 3, `${kind} terminal drain`);
      assert.deepEqual(started, ['first', 'send', 'request']);
    } finally {
      release.resolve();
      await loop.stop();
      await running;
    }
  });
}

test('subscriber connections of one ChannelName share the application gate', async () => {
  const shared = createChannelApplicationDispatchQueue();
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const entered = deferred();
  const release = deferred();
  const firstRecords = [record('first')];
  const secondRecords = [record('send')];
  const started = [];
  const makeSubscriber = (records) => new ZLinkSubscriberReceiveLoop(
    {
      createTopicMessage() { return { topic: '', parts: [] }; },
      createReadablePoller() { return poller(records); }
    },
    {
      subscribe(message) {
        const next = records.shift();
        if (next === undefined) return false;
        message.kind = next.kind;
        return true;
      }
    },
    {
      async dispatch(message) {
        started.push(message.kind);
        if (message.kind === 'first') {
          entered.resolve();
          await release.promise;
        }
      }
    },
    queue,
    undefined,
    undefined,
    undefined,
    shared
  );
  const first = makeSubscriber(firstRecords);
  const second = makeSubscriber(secondRecords);
  const running = [first.run(), second.run()];
  try {
    await entered.promise;
    await until(() => secondRecords.length === 0, 'second subscriber receive');
    assert.deepEqual(started, ['first']);
    release.resolve();
    await until(() => started.length === 2, 'second subscriber dispatch');
    assert.deepEqual(started, ['first', 'send']);
  } finally {
    release.resolve();
    await Promise.all([first.stop(), second.stop()]);
    await Promise.all(running);
  }
});
