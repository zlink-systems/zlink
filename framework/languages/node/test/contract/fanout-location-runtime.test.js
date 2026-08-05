const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const { ZLinkConfigurationException } = require(
  '../../packages/framework/dist/contracts/Configuration/ConfigurationException'
);
const {
  ZLinkChannelSocketRegistry
} = require(
  '../../packages/framework/dist/runtime/channels/channel-socket-registry'
);
const fanoutWire = require(
  '../../packages/framework/dist/runtime/channels/fanout-service-wire'
);
const {
  ZLinkFanoutLocationRuntime
} = require(
  '../../packages/framework/dist/runtime/channels/fanout-location-runtime'
);

test('fanout publishers use dedicated SUB sockets and isolate readiness, protocol failure, and deadline', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: [] },
        publishHandlers: [{ packetName: 'event', handler: { handle() {} } }]
      }
    },
    locations: { useInMemoryStores: true }
  });
  const subscribers = [];
  const monitorHandlers = new Map();
  const terminated = [];
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createSubscriberSocket() {
        const subscriber = fakeSubscriber(`sub-${subscribers.length}`);
        subscribers.push(subscriber);
        return subscriber;
      }
    },
    {},
    {
      openSocketMonitor(subscriber) {
        return {
          nativeInstance: {},
          onEvent(handler) { monitorHandlers.set(subscriber.id, handler); },
          recv() {},
          async dispose() {}
        };
      }
    }
  );
  sockets.openFanoutSubscriberConnection(
    'events',
    'publisher-a:7',
    'tcp://10.0.0.1:9501',
    {
      onReady() {},
      onTerminated(reason) { terminated.push(`a:${reason}`); }
    }
  );
  sockets.openFanoutSubscriberConnection(
    'events',
    'publisher-b:7',
    'tcp://10.0.0.2:9501',
    {
      onReady() {},
      onTerminated(reason) { terminated.push(`b:${reason}`); }
    }
  );
  assert.equal(subscribers.length, 2);
  assert.notEqual(subscribers[0], subscribers[1]);
  for (const handler of monitorHandlers.values()) {
    handler({
      nativeEvent: 'connectionReady',
      localAddr: '',
      remoteAddr: '',
      value: 0
    });
  }

  const base = performance.now();
  assert.equal(sockets.handleFanoutInbound(
    'publisher-a:7',
    {
      topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
      parts: [{ data: () => fanoutWire.FANOUT_LIVENESS_PAYLOAD }]
    },
    subscribers[0],
    base
  ), true);
  assert.equal(sockets.isFanoutConnectionReady('publisher-a:7'), true);
  assert.equal(sockets.isFanoutConnectionReady('publisher-b:7'), false);

  assert.equal(sockets.handleFanoutInbound(
    'publisher-b:7',
    {
      topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
      parts: [{ data: () => fanoutWire.FANOUT_LIVENESS_PAYLOAD }]
    },
    subscribers[1],
    base
  ), true);
  assert.equal(sockets.isFanoutConnectionReady('publisher-b:7'), true);

  let topologyNotifications = 0;
  const topologySource = sockets.fanoutTopologyMonitoringSource('events');
  topologySource.onChange(() => { topologyNotifications += 1; });
  const publisherDescriptor = {
    channelName: 'events',
    publisherRid: 'publisher-b',
    lifecycleGeneration: 7n,
    descriptorRevision: 1n,
    endpoint: 'tcp://10.0.0.2:9501',
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'default',
    ownerId: 'owner-b',
    leaseGeneration: 1n,
    updatedAt: new Date()
  };
  sockets.admitFanoutPublisher(publisherDescriptor, 'publisher-b:7');
  assert.equal(topologyNotifications, 1);
  sockets.removeFanoutPublisher(publisherDescriptor, 'publisher-b:7');
  assert.equal(topologyNotifications, 2);
  await topologySource.dispose();

  sockets.handleFanoutInbound(
    'publisher-a:7',
    {
      topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
      parts: [{ data: () => Uint8Array.of(0) }]
    },
    subscribers[0],
    base + 1
  );
  assert.deepEqual(terminated, ['a:protocol']);
  assert.equal(sockets.isFanoutConnectionReady('publisher-a:7'), false);
  assert.equal(sockets.isFanoutConnectionReady('publisher-b:7'), true);

  sockets.tickClientServerLiveness(base + 15_001);
  assert.deepEqual(terminated, ['a:protocol', 'b:deadline']);
  await sockets.dispose();
});

test('fanout publisher sends the exact reserved beacon every five seconds and public use is rejected', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      events: { routingId: 'publisher', publisher: { bind: 'tcp://127.0.0.1:9501' } }
    },
    locations: { useInMemoryStores: true }
  });
  const published = [];
  const publisher = {
    nativeInstance: {},
    lastEndpoint: 'tcp://127.0.0.1:9501',
    setChannelName() {},
    bind() {},
    onSendReady() {},
    publish(topic, message) {
      published.push({
        topic,
        payload: Buffer.from(message.data())
      });
      return true;
    },
    async dispose() {}
  };
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    { createPublisherSocket() { return publisher; } },
    {}
  );
  sockets.publisher('events');
  sockets.tickClientServerLiveness(performance.now() + 5_001);
  assert.equal(published.length, 1);
  assert.equal(published[0].topic, fanoutWire.FANOUT_LIVENESS_TOPIC);
  assert.deepEqual(
    published[0].payload,
    Buffer.from(fanoutWire.FANOUT_LIVENESS_PAYLOAD)
  );
  assert.throws(
    () => fanoutWire.requirePublicFanoutTopic(
      fanoutWire.FANOUT_LIVENESS_TOPIC
    ),
    (error) => error instanceof ZLinkConfigurationException
      && /reserved/.test(error.message)
  );
  assert.doesNotThrow(
    () => fanoutWire.requirePublicFanoutTopic(
      `${fanoutWire.FANOUT_LIVENESS_TOPIC}.application`
    )
  );
  await sockets.dispose();
});

test('fanout publisher descriptor combines advertise host with the actual bound port', async t => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const stores = locationStores(store);
  const locationRuntime = new internal.ZLinkLocationRuntime({
    stores,
    ownerId: 'fanout-owner'
  });
  await locationRuntime.start('publisher-node');
  const registration = internal.createFrameworkRegistration({
    network: {
      bindHost: '0.0.0.0',
      advertiseHost: 'events.internal'
    },
    channels: {
      events: { routingId: 'publisher', publisher: {} }
    },
    locations: { useInMemoryStores: true }
  });
  const publisher = {
    nativeInstance: {},
    lastEndpoint: 'tcp://0.0.0.0:45123',
    setChannelName() {},
    bind() {},
    onSendReady() {},
    publish() { return true; },
    async dispose() {}
  };
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    { createPublisherSocket() { return publisher; } },
    {}
  );
  const runtime = new ZLinkFanoutLocationRuntime(
    registration,
    sockets,
    locationRuntime,
    stores,
    { pollingIntervalMs: 60_000 },
    () => async () => {}
  );
  t.after(async () => {
    await runtime.stop();
    await locationRuntime.stop();
    await sockets.dispose();
  });

  await runtime.start();
  const rows = await store.listFanoutPublishers('events');
  assert.equal(rows.items.length, 1);
  assert.equal(rows.items[0].endpoint, 'tcp://events.internal:45123');
});

test('automatic fanout reconciles dedicated descriptors by publisher RID and lifecycle', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const stores = locationStores(store);
  const local = new internal.ZLinkLocationRuntime({
    stores,
    ownerId: 'fanout-client'
  });
  await local.start('client-node');
  const remote = await store.claimOwnerLease('publisher-owner', 30_000);
  assert.equal(remote.kind, 'claimed');
  const descriptor = fanoutDescriptor(remote.token);
  assert.equal((await store.updateFanoutPublisher(
    descriptor,
    1
  )).status, 'stored');

  const registration = internal.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: [] },
        publishHandlers: [{ packetName: 'event', handler: { handle() {} } }]
      }
    },
    locations: { useInMemoryStores: true }
  });
  const subscribers = [];
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createSubscriberSocket() {
        const subscriber = fakeSubscriber(`automatic-${subscribers.length}`);
        subscribers.push(subscriber);
        return subscriber;
      }
    },
    {},
    {
      openSocketMonitor() {
        return {
          nativeInstance: {},
          onEvent() {},
          recv() {},
          async dispose() {}
        };
      }
    }
  );
  const runtime = new ZLinkFanoutLocationRuntime(
    registration,
    sockets,
    local,
    stores,
    { pollingIntervalMs: 60_000 },
    () => async () => {}
  );
  await runtime.start();
  assert.equal(subscribers.length, 1);
  assert.equal(runtime.activeTargets('events').length, 0);
  const connectionId = `events\0publisher-a\0${7n.toString()}`;
  sockets.handleFanoutInbound(connectionId, {
    topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
    parts: [{ data: () => fanoutWire.FANOUT_LIVENESS_PAYLOAD }]
  }, subscribers[0]);
  assert.equal(runtime.activeTargets('events').length, 1);
  assert.equal(sockets.fanoutActiveTargets('events').length, 1);

  sockets.handleFanoutInbound(connectionId, {
    topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
    parts: [{ data: () => Uint8Array.of(0) }]
  }, subscribers[0]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(subscribers.length, 2);
  assert.equal(runtime.activeTargets('events').length, 0);
  assert.equal(sockets.fanoutActiveTargets('events').length, 0);
  sockets.handleFanoutInbound(connectionId, {
    topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
    parts: [{ data: () => fanoutWire.FANOUT_LIVENESS_PAYLOAD }]
  }, subscribers[0]);
  assert.equal(runtime.activeTargets('events').length, 0);
  assert.equal(sockets.fanoutActiveTargets('events').length, 0);
  const deadlineBase = performance.now();
  sockets.handleFanoutInbound(connectionId, {
    topic: fanoutWire.FANOUT_LIVENESS_TOPIC,
    parts: [{ data: () => fanoutWire.FANOUT_LIVENESS_PAYLOAD }]
  }, subscribers[1], deadlineBase);
  assert.equal(runtime.activeTargets('events').length, 1);
  assert.equal(sockets.fanoutActiveTargets('events').length, 1);
  sockets.tickClientServerLiveness(deadlineBase + 15_001);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(subscribers.length, 3);
  assert.equal(runtime.activeTargets('events').length, 0);
  assert.equal(sockets.fanoutActiveTargets('events').length, 0);

  await store.updateFanoutPublisher({
    ...descriptor,
    descriptorRevision: 2n,
    state: 2
  }, 2);
  await runtime.tick();
  assert.equal(subscribers.length, 3);
  assert.equal(runtime.activeTargets('events').length, 0);

  await runtime.stop();
  await local.stop();
  await sockets.dispose();
});

test('automatic fanout ignores stale termination callbacks and never reopens a removed target', async t => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const stores = locationStores(store);
  const local = new internal.ZLinkLocationRuntime({
    stores,
    ownerId: 'fanout-client'
  });
  await local.start('client-node');
  const remote = await store.claimOwnerLease('publisher-owner', 30_000);
  assert.equal(remote.kind, 'claimed');
  const descriptor = fanoutDescriptor(remote.token);
  assert.equal((await store.updateFanoutPublisher(descriptor, 1)).status, 'stored');

  const registration = internal.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: [] },
        publishHandlers: [{ packetName: 'event', handler: { handle() {} } }]
      }
    },
    locations: { useInMemoryStores: true }
  });
  const attempts = [];
  const closed = [];
  const admitted = new Set();
  const sockets = {
    openFanoutSubscriberConnection(channelName, connectionId, endpoint, callbacks) {
      const subscriber = fakeSubscriber(`attempt-${attempts.length}`);
      attempts.push({
        channelName,
        connectionId,
        endpoint,
        callbacks,
        subscriber
      });
      return subscriber;
    },
    admitFanoutPublisher(_descriptor, connectionId) {
      admitted.add(connectionId);
      return true;
    },
    removeFanoutPublisher(_descriptor, connectionId) {
      admitted.delete(connectionId);
      return true;
    },
    async closeFanoutSubscriberConnection(connectionId) {
      closed.push(connectionId);
    }
  };
  const runtime = new ZLinkFanoutLocationRuntime(
    registration,
    sockets,
    local,
    stores,
    { pollingIntervalMs: 60_000 },
    () => async () => {}
  );
  t.after(async () => {
    await runtime.stop();
    await local.stop();
  });
  await runtime.start();
  assert.equal(attempts.length, 1);
  assert.equal(admitted.size, 0);

  attempts[0].callbacks.onTerminated('deadline');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(attempts.length, 2);
  assert.equal(closed.length, 1);
  assert.equal(admitted.size, 0);

  // A delayed callback from the replaced physical socket cannot close its successor.
  attempts[0].callbacks.onTerminated('deadline');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(attempts.length, 2);
  assert.equal(closed.length, 1);

  await store.removeFanoutPublisher({
    channelName: descriptor.channelName,
    publisherRid: descriptor.publisherRid
  }, remote.token);
  await runtime.tick();
  assert.equal(closed.length, 2);
  assert.equal(admitted.size, 0);

  // Once reconciliation removes the desired target, its last callback cannot reopen it.
  attempts[1].callbacks.onTerminated('deadline');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(attempts.length, 2);
  assert.equal(closed.length, 2);
});

function fakeSubscriber(id) {
  return {
    id,
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

function locationStores(store) {
  return {
    locationStore: store,
    clientServerStore: store,
    fanoutStore: store,
    peerStore: store,
    spotStore: store,
    actorStore: store,
    routeStore: store,
    ownerLeaseStore: store
  };
}

function fanoutDescriptor(owner) {
  return {
    channelName: 'events',
    publisherRid: 'publisher-a',
    lifecycleGeneration: 7n,
    descriptorRevision: 1n,
    endpoint: 'tcp://10.0.0.1:9501',
    state: 1,
    securityIdentity: 'default',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date(0)
  };
}
