const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkChannelSocketRegistry
} = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
const fanoutWire = require('../../packages/framework/dist/runtime/channels/fanout-service-wire');

test('fanout subscriber without subscribe receives events from different topics', async () => {
  const receivedTopics = [];
  const runtime = await startFanoutPair([], receivedTopics);

  try {
    await publishUntil(runtime.fanout, 'order.created', () => receivedTopics.includes('order.created'));
    await publishUntil(runtime.fanout, 'payment', () => receivedTopics.includes('payment'));

    assert.deepEqual(new Set(receivedTopics), new Set(['order.created', 'payment']));
  } finally {
    await runtime.stop();
  }
});

test('fanout subscriber registered for order receives order.created but not payment', async () => {
  const receivedTopics = [];
  const runtime = await startFanoutPair(['order'], receivedTopics);

  try {
    await publishUntil(
      runtime.fanout,
      'order.created',
      () => receivedTopics.includes('order.created')
    );
    for (let attempt = 0; attempt < 5; attempt += 1) {
      await runtime.fanout.publish(
        'events',
        'payment',
        typedPacket('FanoutSubscriptionEvent', { attempt })
      ).submit();
      await new Promise((resolve) => setTimeout(resolve, 20));
    }
    await new Promise((resolve) => setTimeout(resolve, 100));

    assert.ok(receivedTopics.includes('order.created'));
    assert.equal(receivedTopics.includes('payment'), false);
  } finally {
    await runtime.stop();
  }
});

test('fanout subscriber with application filters becomes ready and stays ready from beacons alone', async () => {
  const registration = framework.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: ['tcp://127.0.0.1:9501'] },
        subscriptions: ['order'],
        publishHandlers: [{ packetName: 'FanoutSubscriptionEvent', handler: { handle() {} } }]
      }
    }
  });
  const subscriber = fakeSubscriber();
  let readyCount = 0;
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    { createSubscriberSocket() { return subscriber; } },
    {},
    { openSocketMonitor() { return fakeMonitor(); } }
  );
  const connectionId = 'events:publisher-a:1';
  sockets.openFanoutSubscriberConnection(
    'events',
    connectionId,
    'tcp://127.0.0.1:9501',
    { onReady() { readyCount += 1; }, onTerminated() {} }
  );

  try {
    assert.deepEqual(subscriber.subscriptions, ['order', fanoutWire.FANOUT_LIVENESS_TOPIC]);
    const base = performance.now();
    sockets.handleFanoutInbound(connectionId, {
      topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
      parts: [{ data: () => fanoutWire.FANOUT_LIVENESS_PAYLOAD }]
    }, subscriber, base);
    assert.equal(sockets.isFanoutConnectionReady(connectionId), true);
    assert.equal(readyCount, 1);

    sockets.tickClientServerLiveness(base + 14_999);
    assert.equal(sockets.isFanoutConnectionReady(connectionId), true);
    assert.equal(readyCount, 1);
  } finally {
    await sockets.dispose();
  }
});

test('fanout publish and subscribe reject the liveness topic prefix and allow adjacent values', () => {
  const event = typedPacket('FanoutSubscriptionEvent', { value: 1 });
  const publisherRegistration = framework.createFrameworkRegistration({
    channels: { events: { publisher: { bind: 'tcp://127.0.0.1:9501' } } }
  });
  const fanout = new framework.DefaultZLinkFanoutClient(publisherRegistration);
  const rejected = [
    fanoutWire.FANOUT_LIVENESS_TOPIC,
    `${fanoutWire.FANOUT_LIVENESS_TOPIC}.application`
  ];
  const allowed = ['\x01ZLF', '\x01ZLF2'];
  const options = framework.createFrameworkOptions((builder) => {
    const channel = builder.addFanoutChannel('events');
    for (const topic of rejected) {
      assert.throws(
        () => channel.subscribe(topic),
        (error) => error instanceof framework.ZLinkConfigurationException
          && /reserved/.test(error.message)
      );
      assert.throws(
        () => fanout.publish('events', topic, event),
        (error) => error instanceof framework.ZLinkConfigurationException
          && /reserved/.test(error.message)
      );
    }
    for (const topic of allowed) {
      assert.doesNotThrow(() => channel.subscribe(topic));
      assert.doesNotThrow(() => fanout.publish('events', topic, event));
    }
  });

  assert.deepEqual(options.channels.events.subscriptions, allowed);
});

test('fanout builder treats duplicate subscribe calls as one subscription', () => {
  const options = framework.createFrameworkOptions((builder) => {
    builder.addFanoutChannel('events')
      .subscribe('order')
      .subscribe('order');
  });

  assert.deepEqual(options.channels.events.subscriptions, ['order']);
});

/**
 * The publisher binds an OS-assigned port; the subscriber connects to the
 * endpoint the publisher's listener status reports after start.
 */
async function startFanoutPair(subscriptions, receivedTopics) {
  const publisherRegistration = framework.createFrameworkRegistration({
    channels: { events: { publisher: { bind: 'tcp://127.0.0.1:*' } } }
  });
  const publisherRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: publisherRegistration });
  const fanout = new framework.DefaultZLinkFanoutClient(
    publisherRegistration,
    publisherRuntime.channelTransport
  );
  await publisherRuntime.start();
  const endpoint = publisherRuntime.getListenerStatus('fanout', 'events').endpoint;
  const subscriberOptions = framework.createFrameworkOptions((builder) => {
    const channel = builder.addFanoutChannel('events').enableSubscriber(endpoint);
    for (const topic of subscriptions) channel.subscribe(topic);
  });
  subscriberOptions.channels.events.publishHandlers = [{
    packetName: 'FanoutSubscriptionEvent',
    handler: {
      handle(_payload, context) {
        receivedTopics.push(context.topic);
      }
    }
  }];
  const subscriberRegistration = framework.createFrameworkRegistration(subscriberOptions);
  const subscriberRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: subscriberRegistration });
  try {
    await subscriberRuntime.start();
  } catch (error) {
    await publisherRuntime.stop();
    throw error;
  }
  return {
    fanout,
    async stop() {
      await subscriberRuntime.stop();
      await publisherRuntime.stop();
    }
  };
}

function typedPacket(packetName, value) {
  const PacketType = { [packetName]: class {} }[packetName];
  return Object.assign(new PacketType(), value);
}

async function publishUntil(fanout, topic, predicate) {
  const deadline = Date.now() + 3_000;
  let attempt = 0;
  while (Date.now() < deadline) {
    await fanout.publish(
      'events',
      topic,
      typedPacket('FanoutSubscriptionEvent', { attempt })
    ).submit();
    if (predicate()) return;
    attempt += 1;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.fail(`fanout topic '${topic}' was not received before timeout`);
}


function fakeSubscriber() {
  return {
    nativeInstance: {},
    subscriptions: [],
    setChannelName() {},
    setSubscription(topic) { this.subscriptions.push(topic); },
    connect() {},
    disconnect() {},
    subscribe() { return false; },
    async dispose() {}
  };
}

function fakeMonitor() {
  return {
    nativeInstance: {},
    onEvent() {},
    recv() {},
    drain() { return 0; },
    async dispose() {}
  };
}
