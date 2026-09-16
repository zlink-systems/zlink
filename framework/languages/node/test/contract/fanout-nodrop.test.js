const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const backend = require('../../packages/framework/dist/runtime/backend');
const framework = require('../../packages/framework/dist/internal');
const {
  submitBindingPublish
} = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
const {
  ZLinkChannelSocketRegistry
} = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');

const TOPIC = 'fanout.nodrop';
const LARGE_PAYLOAD_BYTES = 65_536;
const RECORD_HWM = BigInt(LARGE_PAYLOAD_BYTES + 1_024);
const FILLER = 0x46;
const RELEASED = 0x52;

test('NoDrop omitted keeps lossy fanout so a slow subscriber does not block other subscribers', async () => {
  const harness = await createHarness({ subscriberCount: 2 });
  const [slow, fast] = harness.subscribers;
  const publishedCount = 8;

  try {
    await waitForSubscriptions(harness);
    for (let index = 0; index < publishedCount; index += 1) {
      publish(harness.publisher, index + 1);
      assert.equal(await receiveByteEventually(fast), index + 1);
    }

    const slowValues = drainBytes(slow);
    assert.ok(slowValues.length < publishedCount);
  } finally {
    await harness.dispose();
  }
});

test('NoDrop true admits a record to all matching subscribers only after the slow pipe is ready', async () => {
  const harness = await createHarness({ noDrop: true, subscriberCount: 2 });
  const [slow, fast] = harness.subscribers;

  try {
    await waitForSubscriptions(harness);
    const acceptedFillers = await fillUntilBackpressured(harness.publisher, fast);

    assert.equal(receiveByte(fast), undefined);
    assert.equal(receiveByte(slow), FILLER);

    harness.publisher.options.sendTimeout = 5_000;
    publish(harness.publisher, RELEASED);

    assert.equal(await receiveByteEventually(fast), RELEASED);
    const slowValues = await receiveThroughByte(slow, RELEASED, acceptedFillers);
    assert.equal(slowValues.at(-1), RELEASED);
    assert.equal(slowValues.filter(value => value === FILLER).length, acceptedFillers - 1);
  } finally {
    await harness.dispose();
  }
});

test('NoDrop true maps an exhausted publisher send timeout to DeadlineExceeded', async () => {
  const harness = await createHarness({ noDrop: true, subscriberCount: 1 });

  try {
    await waitForSubscriptions(harness);
    await fillUntilBackpressured(harness.publisher);
    harness.publisher.options.sendTimeout = 25;

    assert.throws(
      () => submitBindingPublish(
        harness.publisher.publish(TOPIC),
        largePayload(RELEASED)
      ),
      (error) => error instanceof framework.ZLinkFrameworkException
        && error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
    );
  } finally {
    await harness.dispose();
  }
});

test('NoDrop configured without a fanout publisher role fails registration startup validation', () => {
  const options = framework.createFrameworkOptions((builder) => {
    builder.addFanoutChannel('events')
      .setNoDrop();
  });

  assert.throws(
    () => framework.createFrameworkRegistration(options),
    (error) => error instanceof framework.ZLinkConfigurationException
      && /NoDrop requires a publisher role/.test(error.message)
  );
});

test('fanout builder NoDrop value is applied to the binding publisher socket option', async () => {
  const harness = await createHarness({ noDrop: true, subscriberCount: 0 });

  try {
    assert.equal(harness.publisher.options.noDrop, true);
  } finally {
    await harness.dispose();
  }
});

test('NoDrop beacon admission failure is reported and retried on the next interval', async () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.addFanoutChannel('events')
        .enablePublisher('inproc://fanout-nodrop-beacon')
        .setNoDrop();
    })
  );
  const failure = new Error('beacon admission deadline');
  const failures = [];
  let attempts = 0;
  const publisher = {
    nativeInstance: {},
    noDrop: false,
    setChannelName() {},
    bind() {},
    publish() {
      attempts += 1;
      if (attempts === 1) throw failure;
    },
    async dispose() {}
  };
  const registry = new ZLinkChannelSocketRegistry(
    registration,
    { createPublisherSocket() { return publisher; } },
    {},
    undefined,
    error => failures.push(error)
  );

  try {
    registry.publisher('events');
    const firstInterval = performance.now() + 5_001;
    registry.tickClientServerLiveness(firstInterval);
    registry.tickClientServerLiveness(firstInterval + 5_001);

    assert.equal(publisher.noDrop, true);
    assert.deepEqual(failures, [failure]);
    assert.equal(attempts, 2);
  } finally {
    await registry.dispose();
  }
});

async function createHarness({ noDrop, subscriberCount }) {
  const endpoint = `inproc://framework-fanout-nodrop-${process.pid}-${Date.now()}-${Math.random()}`;
  const options = framework.createFrameworkOptions((builder) => {
    const channel = builder.addFanoutChannel('events')
      .enablePublisher(endpoint);
    if (noDrop !== undefined) channel.setNoDrop(noDrop);
  });
  const registration = framework.createFrameworkRegistration(options);
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const bindingAdapter = factory.createChannelAdapter();
  const context = bindingAdapter.createContext();
  context.nativeInstance.options.autoHwmEnabled = false;
  const adapter = {
    createPublisherSocket(socketContext) {
      const socket = bindingAdapter.createPublisherSocket(socketContext);
      socket.nativeInstance.options.sendHwm = RECORD_HWM;
      socket.nativeInstance.options.sendTimeout = 0;
      return socket;
    }
  };
  const registry = new ZLinkChannelSocketRegistry(registration, adapter, context);
  const publisher = registry.publisher('events').nativeInstance;
  const subscribers = [];

  try {
    for (let index = 0; index < subscriberCount; index += 1) {
      const subscriber = zlink.createSubSocket(context.nativeInstance);
      subscriber.options.recvHwm = RECORD_HWM;
      subscriber.options.recvTimeout = 100;
      subscriber.options.linger = 0;
      subscriber.setSubscription(TOPIC);
      subscriber.connect(endpoint);
      subscribers.push(subscriber);
    }
  } catch (error) {
    for (const subscriber of subscribers) subscriber.close();
    await registry.dispose();
    await context.dispose();
    throw error;
  }

  return {
    publisher,
    subscribers,
    async dispose() {
      for (const subscriber of subscribers) subscriber.close();
      await registry.dispose();
      await context.dispose();
    }
  };
}

async function waitForSubscriptions(harness) {
  const ready = new Set();
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline && ready.size < harness.subscribers.length) {
    publish(harness.publisher, 0x50, 1_024);
    await delay(5);
    for (let index = 0; index < harness.subscribers.length; index += 1) {
      if (drainBytes(harness.subscribers[index]).includes(0x50)) ready.add(index);
    }
  }
  assert.equal(ready.size, harness.subscribers.length, 'fanout subscriptions did not become ready');
  for (const subscriber of harness.subscribers) drainBytes(subscriber);
}

async function fillUntilBackpressured(publisher, fastSubscriber) {
  for (let accepted = 0; accepted < 64; accepted += 1) {
    try {
      publish(publisher, FILLER);
    } catch (error) {
      assert.equal(error instanceof zlink.SubmitError, true);
      assert.equal(error.result, zlink.SubmitResult.Backpressured);
      return accepted;
    }
    if (fastSubscriber !== undefined) {
      assert.equal(await receiveByteEventually(fastSubscriber), FILLER);
    }
  }
  assert.fail('publisher queue did not reach its configured HWM');
}

async function receiveThroughByte(subscriber, terminal, maximumCount) {
  const received = [];
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline && received.length <= maximumCount) {
    const value = receiveByte(subscriber);
    if (value === undefined) {
      await delay(5);
      continue;
    }
    received.push(value);
    if (value === terminal) return received;
  }
  assert.fail(`subscriber did not receive terminal byte ${terminal}`);
}

async function receiveByteEventually(subscriber) {
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline) {
    const value = receiveByte(subscriber);
    if (value !== undefined) return value;
    await delay(5);
  }
  assert.fail('subscriber did not receive a fanout record');
}

function drainBytes(subscriber) {
  const values = [];
  for (;;) {
    const value = receiveByte(subscriber);
    if (value === undefined) return values;
    values.push(value);
  }
}

function receiveByte(subscriber) {
  const message = new zlink.TopicMessage();
  try {
    if (!subscriber.subscribe(message, zlink.RecvFlags.DontWait)) return undefined;
    return message.parts[0].data()[0];
  } finally {
    message.close();
  }
}

function publish(publisher, value, size = LARGE_PAYLOAD_BYTES) {
  publisher.publish(TOPIC)
    .message(largePayload(value, size))
    .submit();
}

function largePayload(value, size = LARGE_PAYLOAD_BYTES) {
  return Buffer.alloc(size, value);
}

function delay(timeoutMs) {
  return new Promise(resolve => setTimeout(resolve, timeoutMs));
}
