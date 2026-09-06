const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkChannelSocketRegistry
} = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
const {
  ZLinkChannelReceiveLoop
} = require('../../packages/framework/dist/runtime/channels/channel-receive-loops');
const {
  ZLinkChannelOutboundOperations
} = require('../../packages/framework/dist/runtime/channels/channel-outbound-operations');
const {
  ZLinkChannelDispatchServices
} = require('../../packages/framework/dist/runtime/channels/channel-dispatch-services');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkBackendResultError,
  RequestResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  ApplicationJobReceiveFlowController
} = require('../../packages/framework/dist/runtime/application-jobs/receive-flow-controller');
const clientServerWire = require(
  '../../packages/framework/dist/runtime/channels/client-server-service-wire'
);
const submissionResult = require(
  '../../packages/framework/dist/runtime/messaging/submission-result'
);
const routingIds = require('../../packages/framework/dist/runtime/routing-id');
const {
  ZLinkNodeBackendAdapterFactory
} = require('../../packages/framework/dist/runtime/backend/node');

function descriptor(owner, overrides = {}) {
  return {
    channelName: 'orders',
    serverRid: 'server-a',
    lifecycleGeneration: 7n,
    descriptorRevision: 1n,
    endpoint: 'tcp://10.0.0.1:9401',
    weight: 100,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'cluster-a',
    ownerId: owner.token.ownerId,
    leaseGeneration: owner.token.leaseGeneration,
    updatedAt: new Date(0),
    ...overrides
  };
}

function stores(store) {
  return {
    locationStore: store,
    clientServerStore: store,
    peerStore: store,
    spotStore: store,
    actorStore: store,
    routeStore: store,
    ownerLeaseStore: store
  };
}

test('ClientServer service wire preserves opaque server RID bytes and rejects malformed UTF-8 text', () => {
  const owner = { token: { ownerId: 'owner', leaseGeneration: 1n } };
  const opaqueRid = routingIds.decodeRoutingId('opaque-server', 'ff00fe');
  const admitted = clientServerWire.decodeClientServerControl(
    clientServerWire.encodeClientServerAdmit(
      descriptor(owner, { serverRid: opaqueRid }),
      4096
    )
  );
  assert.equal(admitted.kind, 'admit');
  assert.equal(routingIds.encodeRoutingIdStorageHex(admitted.admission.serverRid), 'ff00fe');

  const channelName = 'canonical-utf8-channel';
  const malformed = Buffer.from(clientServerWire.encodeClientServerHello({
    channelName,
    securityIdentity: 'cluster-a',
    normalizedEffectiveMaxMessageBytes: 4096
  }));
  const channelOffset = malformed.indexOf(Buffer.from(channelName, 'utf8'));
  assert.ok(channelOffset > 0);
  malformed[channelOffset] = 0xff;
  assert.throws(
    () => clientServerWire.decodeClientServerControl(malformed),
    error => error.name === 'ZLinkClientServerServiceWireError'
      && /channelName/.test(error.message)
  );

  assert.throws(
    () => clientServerWire.encodeClientServerHello({
      channelName: 'nul\0channel',
      securityIdentity: 'cluster-a',
      normalizedEffectiveMaxMessageBytes: 4096
    }),
    error => error.name === 'ZLinkClientServerServiceWireError'
      && /channelName/.test(error.message)
  );
  assert.throws(
    () => clientServerWire.encodeClientServerHello({
      channelName: '\ud800',
      securityIdentity: 'cluster-a',
      normalizedEffectiveMaxMessageBytes: 4096
    }),
    error => error.name === 'ZLinkClientServerServiceWireError'
      && /canonical UTF-8/.test(error.message)
  );

  const nulText = Buffer.from(clientServerWire.encodeClientServerHello({
    channelName: 'nul-channel',
    securityIdentity: 'cluster-a',
    normalizedEffectiveMaxMessageBytes: 4096
  }));
  const nulOffset = nulText.indexOf(Buffer.from('nul-channel', 'utf8'));
  assert.ok(nulOffset > 0);
  nulText[nulOffset] = 0;
  assert.throws(
    () => clientServerWire.decodeClientServerControl(nulText),
    error => error.name === 'ZLinkClientServerServiceWireError'
      && /channelName/.test(error.message)
  );

  assert.throws(
    () => clientServerWire.encodeClientServerAdmit(
      descriptor(owner, { serverRid: { toHex: () => 'aa'.repeat(256) } }),
      4096
    ),
    error => error.name === 'ZLinkClientServerServiceWireError'
      && /serverRid/.test(error.message)
  );
});

test('dedicated ClientServer descriptor store fences immutable identity and mutable revision', async () => {
  const store = new internal.ZLinkInMemoryLocationStore(
    () => new Date(Date.UTC(2026, 6, 23, 0, 0, 0))
  );
  const owner = await store.claimOwnerLease('server-owner', 30_000);
  assert.equal(owner.kind, 'claimed');

  const claimed = await store.updateClientServer(
    descriptor(owner),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  assert.equal(claimed.status, internal.ZLinkLocationWriteStatus.Stored);

  const stale = await store.updateClientServer(
    descriptor(owner, { descriptorRevision: 1n, weight: 50 }),
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(stale.status, internal.ZLinkLocationWriteStatus.IgnoredStale);

  const renewed = await store.updateClientServer(
    descriptor(owner, { descriptorRevision: 2n, weight: 50 }),
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(renewed.status, internal.ZLinkLocationWriteStatus.Stored);
  const page = await store.listClientServers('orders', { pageSize: 1 });
  assert.equal(page.items.length, 1);
  assert.equal(page.items[0].weight, 50);
  assert.equal(page.items[0].descriptorRevision, 2n);

  assert.equal(
    await store.removeClientServer(
      { channelName: 'orders', serverRid: 'server-a' },
      owner.token
    ),
    internal.ZLinkLocationWriteStatus.Stored
  );
  assert.equal((await store.listClientServers('orders')).items.length, 0);
});

test('automatic ClientServer startup requires the minimal Location Store provider SPI', () => {
  assert.throws(() => internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { storeInstance: {} }
  }), /Location Store must implement read, write, and scan/);
});

test('ClientServer socket identity advertises the concrete port returned after bind', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        server: {
          bind: 'tcp://0.0.0.0:0',
          advertiseHost: 'orders.internal'
        },
        sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }]
      }
    }
  });
  const router = {
    nativeInstance: {},
    lastEndpoint: undefined,
    peerWeight: 100,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: -1,
    maxMessageSize: -1,
    setChannelName() {},
    setRoutingId(value) { this.routingId = value; },
    bind(endpoint) {
      assert.equal(endpoint, 'tcp://0.0.0.0:0');
      this.lastEndpoint = 'tcp://0.0.0.0:49152';
    },
    connect() {},
    disconnect() {},
    async dispose() {}
  };
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    { createRouterSocket() { return router; } },
    {}
  );

  const identity = sockets.clientServerServerIdentity('orders');
  assert.equal(identity.endpoint, 'tcp://orders.internal:49152');
  assert.equal(identity.serverRid, router.routingId);
  assert.ok(identity.lifecycleGeneration > 0n);
  await sockets.dispose();
});

test('paired ClientServer DEALER and ROUTER sockets receive absolute queue pressure state', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        client: { manualConnections: [] },
        server: { bind: 'tcp://127.0.0.1:0' },
        sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }]
      }
    }
  });
  const calls = [];
  const socket = (kind) => ({
    nativeInstance: {},
    peerWeight: 100,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: -1,
    maxMessageSize: -1,
    setReceiveFlowState(state) { calls.push(`${kind}:flow:${state}`); },
    setChannelName() {},
    setRoutingId() {},
    bind() { calls.push(`${kind}:bind`); },
    connect() {},
    disconnect() {},
    async dispose() { calls.push(`${kind}:dispose`); }
  });
  const dealer = socket('dealer');
  const router = socket('router');
  let now = 0;
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({
      maxQueuedApplicationJobs: 5n,
      pauseThresholdPercent: 80,
      resumeThresholdPercent: 40
    }, () => 1n),
    () => now
  );
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() { return dealer; },
      createRouterSocket() { return router; }
    },
    {},
    undefined,
    undefined,
    queue
  );

  sockets.clientDealer('orders');
  sockets.channelRouter('orders');
  assert.deepEqual(calls.slice(0, 3), ['dealer:flow:0', 'router:flow:0', 'router:bind']);
  const permits = [];
  for (let index = 0; index < 4; index += 1) permits.push(await queue.acquire());
  assert.deepEqual(calls.slice(-2), ['dealer:flow:1', 'router:flow:1']);
  permits.pop().releaseAfterInternalProcessing();
  assert.equal(queue.pressureState(), 'paused');
  permits.pop().releaseAfterInternalProcessing();
  assert.equal(queue.pressureState(), 'running');
  assert.deepEqual(calls.slice(-2), ['dealer:flow:0', 'router:flow:0']);
  for (const permit of permits) permit.releaseAfterInternalProcessing();

  await sockets.dispose();
  const callCountAfterDispose = calls.length;
  const afterDispose = [];
  for (let index = 0; index < 4; index += 1) afterDispose.push(await queue.acquire());
  assert.equal(calls.length, callCountAfterDispose);
  for (const permit of afterDispose) permit.releaseAfterInternalProcessing();
});

test('queue-owned receive-flow transitions serialize reentrant listeners without duplicate native calls', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: { client: { manualConnections: ['tcp://127.0.0.1:9490'] } },
      payments: { client: { manualConnections: ['tcp://127.0.0.1:9491'] } }
    }
  });
  const calls = [];
  const permits = [];
  let releaseDuringPause = true;
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({
      maxQueuedApplicationJobs: 5n,
      pauseThresholdPercent: 80,
      resumeThresholdPercent: 40
    }, () => 1n)
  );
  const dealer = (kind, reentrant) => ({
    nativeInstance: {},
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: -1,
    maxMessageSize: -1,
    setReceiveFlowState(state) {
      calls.push(`${kind}:${state}`);
      if (reentrant && state === 1 && releaseDuringPause) {
        releaseDuringPause = false;
        permits.shift().releaseAfterInternalProcessing();
        permits.shift().releaseAfterInternalProcessing();
      }
    },
    setChannelName() {},
    setRoutingId() {},
    connect() {},
    disconnect() {},
    async dispose() {}
  });
  const dealers = [dealer('orders', true), dealer('payments', false)];
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    { createDealerSocket() { return dealers.shift(); } },
    {},
    undefined,
    undefined,
    queue
  );
  sockets.clientDealer('orders');
  sockets.clientDealer('payments');
  for (let index = 0; index < 4; index += 1) permits.push(await queue.acquire());

  assert.equal(queue.pressureState(), 'running');
  assert.deepEqual(calls, ['orders:0', 'payments:0', 'orders:1', 'orders:0']);

  for (const permit of permits) permit.releaseAfterInternalProcessing();
  assert.deepEqual(calls, ['orders:0', 'payments:0', 'orders:1', 'orders:0']);
  await sockets.dispose();
});

test('initial receive-flow configuration failures are counted and prevent socket exposure', () => {
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: ['tcp://127.0.0.1:9401'] } } }
  });
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 5n }, () => 1n)
  );
  const failures = [];
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() {
        return {
          nativeInstance: {},
          setReceiveFlowState() { throw new Error('flow config failed'); },
          async dispose() {}
        };
      }
    },
    {},
    undefined,
    error => failures.push(error),
    queue
  );
  assert.throws(() => sockets.clientDealer('orders'), /flow config failed/);
  assert.equal(queue.snapshot().flowStateConfigFailureCount, 1n);
  assert.deepEqual(failures.map(error => error.message), ['flow config failed']);
});

test('receive-flow controller fences reentrant disposal during initial state apply', () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 5n }, () => 1n)
  );
  const controller = new ApplicationJobReceiveFlowController(
    queue,
    state => state === 'paused' ? 1 : 0
  );
  let calls = 0;
  const target = {
    setReceiveFlowState() {
      calls += 1;
      controller.dispose();
    }
  };

  assert.throws(() => controller.register(target), /controller is disposed/);
  assert.equal(calls, 1);
  assert.throws(() => controller.register(target), /controller is disposed/);
  assert.equal(queue.snapshot().flowStateConfigFailureCount, 0n);
});

test('RouteMesh ROUTER receives current absolute pressure before connect and bind', async () => {
  const registration = internal.createFrameworkRegistration({
    routeChannels: [{
      routerChannelId: 'play.route',
      bind: 'tcp://127.0.0.1:9402',
      manualConnections: ['tcp://127.0.0.1:9403']
    }]
  });
  const calls = [];
  const router = {
    nativeInstance: {},
    peerWeight: 100,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: -1,
    maxMessageSize: -1,
    setReceiveFlowState(state) { calls.push(`flow:${state}`); },
    setChannelName() {},
    setRoutingId() {},
    setProbe() {},
    connect() { calls.push('connect'); },
    bind() { calls.push('bind'); },
    async dispose() { calls.push('dispose'); }
  };
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({
      maxQueuedApplicationJobs: 5n,
      pauseThresholdPercent: 80,
      resumeThresholdPercent: 40
    }, () => 1n)
  );
  const permits = [];
  for (let index = 0; index < 4; index += 1) permits.push(await queue.acquire());
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    { createRouterSocket() { return router; } },
    {},
    undefined,
    undefined,
    queue
  );
  sockets.routeRouter('play.route');
  assert.deepEqual(calls.slice(0, 3), ['flow:1', 'connect', 'bind']);
  permits.pop().releaseAfterInternalProcessing();
  permits.pop().releaseAfterInternalProcessing();
  assert.equal(calls.at(-1), 'flow:0');
  for (const permit of permits) permit.releaseAfterInternalProcessing();
  await sockets.dispose();
});

test('same-process ClientServer uses local bound endpoint without a Location Store', async () => {
  const createLocal = (weight) => {
    const registration = internal.createFrameworkRegistration({
      channels: {
        orders: {
          client: { manualConnections: [] },
          server: { bind: 'tcp://0.0.0.0:0', advertiseHost: '127.0.0.1', weight },
          sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }]
        }
      }
    });
    const router = {
      ...fakeDealer('local-router'),
      bind() { this.lastEndpoint = 'tcp://0.0.0.0:49152'; }
    };
    const dealer = fakeDealer('local-dealer');
    const requests = [];
    let dealerMonitor;
    dealer.connect = endpoint => { dealer.connected = endpoint; };
    dealer.request = message => new Promise((resolve, reject) => {
      requests.push({ frame: Buffer.from(message.data()), resolve, reject });
    });
    const sockets = new ZLinkChannelSocketRegistry(
      registration,
      {
        createRouterSocket() { return router; },
        createDealerSocket() { return dealer; },
        createReadablePoller() { return readyPoller(); }
      },
      {},
      {
        openSocketMonitor(socket) {
          return {
            nativeInstance: {},
            onEvent(handler) {
              if (socket === dealer) dealerMonitor = handler;
            },
            async dispose() {}
          };
        }
      }
    );
    sockets.startLocalClientServerConnections();
    return { sockets, dealer, requests, dealerMonitor };
  };

  for (const [weight, expectedReady] of [[75, true], [0, false]]) {
    const local = createLocal(weight);
    const identity = local.sockets.clientServerServerIdentity('orders');
    assert.equal(local.dealer.connected, identity.endpoint);
    assert.equal(local.sockets.clientDealerForOutbound('orders'), undefined);

    local.dealerMonitor({
      nativeEvent: internal.ZLinkSocketNativeEventType.ConnectionReady,
      routingId: identity.serverRid,
      remoteAddr: identity.endpoint
    });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(
      clientServerWire.decodeClientServerControl(local.requests[0].frame).kind,
      'hello'
    );
    local.requests[0].resolve([
      zlink.Message.from(clientServerWire.encodeClientServerAdmit({
        ...descriptor({ token: { ownerId: 'local', leaseGeneration: 1n } }),
        serverRid: identity.serverRid,
        lifecycleGeneration: identity.lifecycleGeneration,
        endpoint: identity.endpoint,
        weight,
        securityIdentity: 'default'
      }, 4096))
    ]);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(
      local.sockets.clientDealerForOutbound('orders') === local.dealer,
      expectedReady
    );
    await local.sockets.dispose();
  }
});

test('production ClientServer outbound socket selection uses admitted descriptor weights', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const dealers = [];
  const adapter = {
    createDealerSocket() {
      const dealer = fakeDealer(`dealer-${dealers.length}`);
      dealers.push(dealer);
      return dealer;
    },
    createReadablePoller() { return readyPoller(); }
  };
  const monitoringAdapter = {
    openSocketMonitor() {
      return {
        nativeInstance: {},
        onEvent() {},
        drain() { return 0; },
        async dispose() {}
      };
    }
  };
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    adapter,
    {},
    monitoringAdapter
  );
  sockets.openClientServerConnection(
    'orders',
    'orders-a:7',
    'tcp://10.0.0.1:9401',
    { onTransportReady() {}, onTerminated() {} }
  );
  sockets.openClientServerConnection(
    'orders',
    'orders-b:7',
    'tcp://10.0.0.2:9401',
    { onTransportReady() {}, onTerminated() {} }
  );
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
  sockets.admitClientServerConnection(discoveryDescriptor('server-a', 1), 'orders-a:7');
  sockets.admitClientServerConnection(discoveryDescriptor('server-b', 3), 'orders-b:7');

  const selected = Array.from(
    { length: 8 },
    () => sockets.clientDealerForOutbound('orders').id
  );
  assert.deepEqual(selected, [
    'dealer-1',
    'dealer-0',
    'dealer-1',
    'dealer-1',
    'dealer-1',
    'dealer-0',
    'dealer-1',
    'dealer-1'
  ]);
  sockets.admitClientServerConnection({
    ...discoveryDescriptor('server-a', 0),
    descriptorRevision: 2n
  }, 'orders-a:7');
  assert.deepEqual(
    Array.from({ length: 4 }, () => sockets.clientDealerForOutbound('orders').id),
    ['dealer-1', 'dealer-1', 'dealer-1', 'dealer-1']
  );
  sockets.admitClientServerConnection({
    ...discoveryDescriptor('server-b', 3),
    descriptorRevision: 2n,
    state: 'retiring'
  }, 'orders-b:7');
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
  await sockets.dispose();
});

test('ClientServer socket creation releases the Poller and dealer when monitor setup fails', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const dealer = fakeDealer('monitor-failure');
  let dealerDisposed = false;
  dealer.dispose = async () => {
    dealerDisposed = true;
  };
  let pollerDisposed = 0;
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() { return dealer; },
      createReadablePoller() {
        return {
          wait() { return false; },
          dispose() { pollerDisposed += 1; }
        };
      }
    },
    {},
    {
      openSocketMonitor() {
        throw new Error('monitor setup failed');
      }
    }
  );

  assert.throws(
    () => sockets.openClientServerConnection(
      'orders',
      'monitor-failure',
      'tcp://10.0.0.1:9401',
      { onTransportReady() {}, onTerminated() {} }
    ),
    /monitor setup failed/
  );
  assert.equal(pollerDisposed, 1);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(dealerDisposed, true);
  await sockets.dispose();
});

test('automatic and manual ClientServer sources share one physical connection until the last alias closes', async () => {
  const createSockets = () => {
    const registration = internal.createFrameworkRegistration({
      channels: { orders: { client: { manualConnections: [] } } },
      locations: { useInMemoryStores: true }
    });
    const dealers = [];
    const sockets = new ZLinkChannelSocketRegistry(
      registration,
      {
        createDealerSocket() {
          const dealer = fakeDealer(`physical-${dealers.length}`);
          dealer.disposed = false;
          dealer.dispose = async () => { dealer.disposed = true; };
          dealers.push(dealer);
          return dealer;
        },
        createReadablePoller() { return readyPoller(); }
      },
      {},
      {
        openSocketMonitor() {
          return { nativeInstance: {}, onEvent() {}, drain() { return 0; }, async dispose() {} };
        }
      }
    );
    return { sockets, dealers };
  };
  const descriptor = discoveryDescriptor('server-a', 100);

  const forward = createSockets();
  forward.sockets.openClientServerConnection(
    'orders', 'automatic', descriptor.advertisedEndpoint,
    { onTransportReady() {}, onTerminated() {} }
  );
  forward.sockets.openClientServerConnection(
    'orders', 'manual', descriptor.advertisedEndpoint,
    { onTransportReady() {}, onTerminated() {} }
  );
  assert.equal(forward.sockets.admitClientServerConnection(descriptor, 'automatic'), true);
  assert.equal(forward.sockets.admitClientServerConnection(descriptor, 'manual'), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(forward.dealers[1].disposed, true);
  assert.equal(forward.sockets.clientDealerForOutbound('orders'), forward.dealers[0]);
  await forward.sockets.closeClientServerConnection('manual');
  assert.equal(forward.dealers[0].disposed, false);
  assert.equal(forward.sockets.clientDealerForOutbound('orders'), forward.dealers[0]);
  await forward.sockets.dispose();

  const reverse = createSockets();
  reverse.sockets.openClientServerConnection(
    'orders', 'manual', descriptor.advertisedEndpoint,
    { onTransportReady() {}, onTerminated() {} }
  );
  reverse.sockets.openClientServerConnection(
    'orders', 'automatic', descriptor.advertisedEndpoint,
    { onTransportReady() {}, onTerminated() {} }
  );
  assert.equal(reverse.sockets.admitClientServerConnection(descriptor, 'manual'), true);
  assert.equal(reverse.sockets.admitClientServerConnection(descriptor, 'automatic'), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(reverse.dealers[1].disposed, true);
  await reverse.sockets.closeClientServerConnection('manual');
  assert.equal(reverse.dealers[0].disposed, false);
  assert.equal(reverse.sockets.clientDealerForOutbound('orders'), reverse.dealers[0]);
  await reverse.sockets.dispose();
});

test('manual ClientServer endpoints use dedicated monitored admission and reconnect', async () => {
  const endpoint = 'tcp://10.0.0.1:9401';
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [endpoint] } } }
  });
  const dealers = [];
  const connects = [];
  let monitorHandler;
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() {
        const dealer = fakeDealer(`manual-${dealers.length}`);
        dealer.requests = [];
        dealer.connect = value => { dealer.connected = value; connects.push(value); };
        dealer.request = message => new Promise((resolve, reject) => {
          dealer.requests.push({ frame: Buffer.from(message.data()), resolve, reject });
        });
        dealers.push(dealer);
        return dealer;
      },
      createReadablePoller() { return readyPoller(); }
    },
    {},
    {
      openSocketMonitor() {
        return {
          nativeInstance: {},
          onEvent(handler) { monitorHandler = handler; },
          drain() { return 0; },
          async dispose() {}
        };
      }
    }
  );

  sockets.startManualClientServerConnections();
  assert.equal(dealers.length, 1);
  assert.equal(dealers[0].connected, endpoint);
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);

  monitorHandler({
    nativeEvent: internal.ZLinkSocketNativeEventType.ConnectionReady,
    routingId: 'server-a',
    remoteAddr: endpoint
  });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(clientServerWire.decodeClientServerControl(dealers[0].requests[0].frame).kind, 'hello');
  dealers[0].requests[0].resolve([
    zlink.Message.from(clientServerWire.encodeClientServerAdmit({
      ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
      securityIdentity: 'default'
    }, 4096))
  ]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(sockets.clientDealerForOutbound('orders'), dealers[0]);

  monitorHandler({
    nativeEvent: internal.ZLinkSocketNativeEventType.Disconnected,
    routingId: 'server-a',
    remoteAddr: endpoint
  });
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
  monitorHandler({
    nativeEvent: internal.ZLinkSocketNativeEventType.ConnectionReady,
    routingId: 'server-a',
    remoteAddr: endpoint
  });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(dealers[0].requests.length, 2);
  assert.deepEqual(connects, [endpoint]);
  await sockets.dispose();
});

for (const cause of ['malformed control', 'invalid pushed control', 'liveness deadline']) {
  test(`ClientServer ${cause} restores intent only after its endpoint closes`, async () => {
    const endpoint = 'tcp://10.0.0.1:9401';
    const registration = internal.createFrameworkRegistration({
      channels: { orders: { client: { manualConnections: [endpoint] } } }
    });
    const dealer = fakeDealer('termination');
    const calls = [];
    const inbound = [];
    const diagnostics = [];
    const requests = [];
    dealer.connect = value => calls.push(`connect:${value}`);
    dealer.disconnect = value => calls.push(`disconnect:${value}`);
    dealer.recv = () => inbound.shift();
    dealer.request = message => new Promise(resolve => {
      requests.push({ frame: Buffer.from(message.data()), resolve });
    });
    let onEvent;
    const sockets = new ZLinkChannelSocketRegistry(registration, {
      createDealerSocket() { return dealer; },
      createReadablePoller() { return readyPoller(); }
    }, {}, {
      openSocketMonitor() {
        return {
          nativeInstance: {},
          onEvent(handler) { onEvent = handler; },
          drain() { return 0; },
          async dispose() {}
        };
      }
    }, error => diagnostics.push(error));
    const emit = (nativeEvent, remoteAddr = endpoint, value = 1n) => onEvent({
      nativeEvent, remoteAddr, value, routingId: 'server-a'
    });
    const events = internal.ZLinkSocketNativeEventType;
    const admit = async index => {
      assert.equal(clientServerWire.decodeClientServerControl(requests[index].frame).kind, 'hello');
      requests[index].resolve([zlink.Message.from(clientServerWire.encodeClientServerAdmit(
        descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }, {
          securityIdentity: 'default'
        }), 4096
      ))]);
      await new Promise(resolve => setImmediate(resolve));
    };
    try {
      sockets.startManualClientServerConnections();
      emit(events.ConnectionReady);
      await admit(0);
      assert.equal(sockets.clientDealerForOutbound('orders'), dealer);
      if (cause !== 'liveness deadline') {
        inbound.push(receivedControl(cause === 'malformed control'
          ? clientServerWire.encodeClientServerLivenessProbe(1n).subarray(0, 5)
          : clientServerWire.encodeClientServerReject(1)));
        sockets.tickClientServerLiveness();
        assert.equal(diagnostics.length, 1);
        assert.match(diagnostics[0].message, /probeId is truncated|invalid pushed control record/);
      } else {
        sockets.tickClientServerLiveness(performance.now() + 15_001);
      }
      assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
      assert.deepEqual(calls, [`connect:${endpoint}`, `disconnect:${endpoint}`]);
      emit(events.ConnectionReady);
      emit(events.Closed, 'tcp://10.0.0.2:9401');
      emit(events.HandshakeFailedProtocol);
      sockets.tickClientServerLiveness(performance.now() + 30_001);
      assert.equal(requests.length, 1);
      assert.equal(calls.length, 2);

      emit(events.Disconnected);
      assert.deepEqual(calls, [
        `connect:${endpoint}`, `disconnect:${endpoint}`, `connect:${endpoint}`
      ]);
      assert.equal(requests.length, 1);
      assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
      emit(events.Closed);
      assert.equal(calls.length, 3);
      emit(events.ConnectionReady, endpoint, 0n);
      assert.equal(requests.length, 1);
      emit(events.ConnectionReady);
      assert.equal(requests.length, 2);
      await admit(1);
      assert.equal(sockets.clientDealerForOutbound('orders'), dealer);
      assert.equal(calls.length, 3);
    } finally {
      await sockets.dispose();
    }
  });
}

for (const transport of ['inproc', 'tcp']) {
  test(`native ${transport} ClientServer readmits malformed pushed control without a connect loop`, async () => {
    const factory = new ZLinkNodeBackendAdapterFactory();
    const adapter = factory.createChannelAdapter();
    const monitoring = factory.createMonitoringAdapter();
    const context = adapter.createContext();
    const router = adapter.createRouterSocket(context);
    const calls = [];
    const diagnostics = [];
    let dealerCount = 0;
    let helloCount = 0;
    let clientRid;
    router.nativeInstance.options.handover = true;
    router.setRoutingId('server-a');
    router.bind(transport === 'tcp' ? 'tcp://127.0.0.1:0' : 'inproc://cs-reconnect-intent');
    const endpoint = router.lastEndpoint;
    const registration = internal.createFrameworkRegistration({
      channels: { orders: { client: { manualConnections: [endpoint] } } }
    });
    const createDealer = adapter.createDealerSocket.bind(adapter);
    adapter.createDealerSocket = value => {
      dealerCount += 1;
      const dealer = createDealer(value);
      for (const action of ['connect', 'disconnect']) {
        const perform = dealer[action].bind(dealer);
        dealer[action] = address => {
          calls.push({ kind: action, endpoint: address });
          perform(address);
        };
      }
      return dealer;
    };
    const openMonitor = monitoring.openSocketMonitor.bind(monitoring);
    monitoring.openSocketMonitor = socket => {
      const monitor = openMonitor(socket);
      monitor.onEvent(event => calls.push({
        kind: event.nativeEvent, endpoint: event.remoteAddr, value: String(event.value)
      }));
      return monitor;
    };
    const sockets = new ZLinkChannelSocketRegistry(
      registration, adapter, context, monitoring, error => diagnostics.push(error)
    );
    const readmit = async expectedHellos => {
      const deadline = Date.now() + 3000;
      while (Date.now() < deadline) {
        sockets.tickClientServerLiveness();
        const received = router.recv(1);
        if (received !== undefined) {
          try {
            assert.equal(received.parts.length, 1);
            assert.equal(clientServerWire.decodeClientServerControl(received.parts[0].data()).kind, 'hello');
            clientRid = received.routingId;
            helloCount += 1;
            const reply = zlink.Message.from(clientServerWire.encodeClientServerAdmit(
              descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }, {
                endpoint, securityIdentity: 'default'
              }), 4096
            ));
            try {
              router.reply(received.routingId, received.replyToken, reply);
            } finally {
              reply.close();
            }
          } finally {
            received.close();
          }
        }
        if (helloCount === expectedHellos && sockets.clientDealerForOutbound('orders') !== undefined) return;
        await new Promise(resolve => setTimeout(resolve, 5));
      }
      assert.fail(`admission did not finish: ${JSON.stringify({ helloCount, calls, diagnostics: diagnostics.map(error => error.message) })}`);
    };
    try {
      sockets.startManualClientServerConnections();
      await readmit(1);
      const malformed = zlink.Message.from(
        clientServerWire.encodeClientServerLivenessProbe(1n).subarray(0, 5)
      );
      try {
        await router.send(clientRid, malformed);
      } finally {
        malformed.close();
      }
      await readmit(2);
      assert.equal(dealerCount, 1);
      assert.equal(helloCount, 2);
      assert.equal(diagnostics.length, 1);
      assert.match(diagnostics[0].message, /probeId is truncated/);
      assert.deepEqual(calls.filter(value => typeof value.kind === 'string'), [
        { kind: 'connect', endpoint },
        { kind: 'disconnect', endpoint },
        { kind: 'connect', endpoint }
      ]);
      const disconnect = calls.findIndex(value => value.kind === 'disconnect');
      const closed = calls.findIndex((value, index) => index > disconnect
        && value.endpoint === endpoint
        && [internal.ZLinkSocketNativeEventType.Disconnected, internal.ZLinkSocketNativeEventType.Closed].includes(value.kind));
      const restored = calls.findIndex((value, index) => index > disconnect && value.kind === 'connect');
      const ready = calls.findIndex((value, index) => index > restored
        && value.kind === internal.ZLinkSocketNativeEventType.ConnectionReady);
      assert.ok(disconnect > 0 && closed > disconnect && restored > closed && ready > restored);
    } finally {
      await sockets.dispose();
      await router.dispose();
      await context.dispose();
    }
  });
}

test('manual ClientServer reconnect fences a late admission from the previous physical pipe', async () => {
  const endpoint = 'tcp://10.0.0.1:9401';
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [endpoint] } } }
  });
  const dealer = fakeDealer('manual-race');
  const requests = [];
  dealer.request = message => new Promise((resolve, reject) => {
    requests.push({ frame: Buffer.from(message.data()), resolve, reject });
  });
  let monitorHandler;
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() { return dealer; },
      createReadablePoller() { return readyPoller(); }
    },
    {},
    {
      openSocketMonitor() {
        return {
          nativeInstance: {},
          onEvent(handler) { monitorHandler = handler; },
          drain() { return 0; },
          async dispose() {}
        };
      }
    }
  );
  sockets.startManualClientServerConnections();
  monitorHandler({
    nativeEvent: internal.ZLinkSocketNativeEventType.ConnectionReady,
    routingId: 'server-a',
    remoteAddr: endpoint
  });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(requests.length, 1);

  monitorHandler({
    nativeEvent: internal.ZLinkSocketNativeEventType.Disconnected,
    routingId: 'server-a',
    remoteAddr: endpoint
  });
  monitorHandler({
    nativeEvent: internal.ZLinkSocketNativeEventType.ConnectionReady,
    routingId: 'server-a',
    remoteAddr: endpoint
  });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(requests.length, 2);

  const admitted = {
    ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
    securityIdentity: 'default'
  };
  requests[0].resolve([
    zlink.Message.from(clientServerWire.encodeClientServerAdmit(admitted, 4096))
  ]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);

  requests[1].resolve([
    zlink.Message.from(clientServerWire.encodeClientServerAdmit(admitted, 4096))
  ]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(sockets.clientDealerForOutbound('orders'), dealer);
  await sockets.dispose();
});

test('ClientServer liveness ACK is fenced to the current probe and application traffic does not refresh it', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const dealer = fakeDealer('liveness');
  const requests = [];
  const inbound = [];
  const sent = [];
  const diagnostics = [];
  dealer.recv = () => inbound.shift();
  dealer.send = async message => {
    sent.push(Buffer.from(message.data()));
  };
  dealer.request = message => new Promise((resolve, reject) => {
    requests.push({ frame: Buffer.from(message.data()), resolve, reject });
  });
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() { return dealer; },
      createReadablePoller() { return readyPoller(); }
    },
    {},
    {
      openSocketMonitor() {
        return { nativeInstance: {}, onEvent() {}, drain() { return 0; }, async dispose() {} };
      }
    },
    error => diagnostics.push(error)
  );
  sockets.openClientServerConnection(
    'orders',
    'connection-a',
    'tcp://10.0.0.1:9401',
    { onTransportReady() {}, onTerminated() {} }
  );
  sockets.admitClientServerConnection(discoveryDescriptor('server-a', 100), 'connection-a');

  const base = performance.now();
  inbound.push(receivedControl(clientServerWire.encodeClientServerLivenessProbe(91n)));
  await sockets.tickClientServerLiveness(base);
  const serverProbeAck = clientServerWire.decodeClientServerControl(sent[0]);
  assert.equal(serverProbeAck.kind, 'livenessAck');
  assert.equal(serverProbeAck.probeId, 91n);
  await sockets.tickClientServerLiveness(base + 5_001);
  const probe = clientServerWire.decodeClientServerControl(requests[0].frame);
  assert.equal(probe.kind, 'livenessProbe');
  await sockets.tickClientServerLiveness(base + 10_002);
  const retransmit = clientServerWire.decodeClientServerControl(requests[1].frame);
  assert.equal(retransmit.kind, 'livenessProbe');
  assert.equal(retransmit.probeId, probe.probeId);
  requests[0].resolve([
    zlink.Message.from(clientServerWire.encodeClientServerLivenessAck(probe.probeId + 1n))
  ]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(diagnostics.length, 1);
  assert.match(diagnostics[0].message, /stale or duplicate liveness ACK/);
  await sockets.tickClientServerLiveness(base + 15_001);
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
  await sockets.dispose();
});

test('ClientServer pushed descriptor updates reject stale and conflicting revisions as protocol errors', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const dealer = fakeDealer('updates');
  const inbound = [];
  dealer.recv = () => inbound.shift();
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() { return dealer; },
      createReadablePoller() { return readyPoller(); }
    },
    {},
    {
      openSocketMonitor() {
        return { nativeInstance: {}, onEvent() {}, drain() { return 0; }, async dispose() {} };
      }
    }
  );
  sockets.openClientServerConnection(
    'orders',
    'connection-a',
    'tcp://10.0.0.1:9401',
    { onTransportReady() {}, onTerminated() {} }
  );
  sockets.admitClientServerConnection(discoveryDescriptor('server-a', 100), 'connection-a');
  inbound.push(receivedControl(clientServerWire.encodeClientServerUpdate({
    ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
    descriptorRevision: 2n,
    weight: 25,
    securityIdentity: 'cluster-a'
  }, 1024)));
  await sockets.tickClientServerLiveness();
  assert.equal(sockets.clientServerActiveTargets('orders')[0].weight, 25);

  const connection = sockets.clientServerConnections.get('connection-a');
  assert.throws(() => sockets.applyClientServerDescriptorUpdate(
    'connection-a',
    connection,
    clientServerWire.decodeClientServerControl(clientServerWire.encodeClientServerUpdate({
      ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
      descriptorRevision: 3n,
      weight: 25,
      securityIdentity: 'cluster-a'
    }, 2048)).admission
  ), error => error.name === 'ServiceWireProtocolError' && /message bound/.test(error.message));
  assert.equal(sockets.clientServerActiveTargets('orders')[0].weight, 25);

  assert.throws(() => sockets.applyClientServerDescriptorUpdate(
    'connection-a',
    connection,
    clientServerWire.decodeClientServerControl(clientServerWire.encodeClientServerUpdate({
    ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
    descriptorRevision: 1n,
    weight: 100,
    securityIdentity: 'cluster-a'
    }, 1024)).admission
  ), error => error.name === 'ServiceWireProtocolError' && /stale/.test(error.message));
  assert.equal(sockets.clientServerActiveTargets('orders')[0].weight, 25);

  assert.throws(() => sockets.applyClientServerDescriptorUpdate(
    'connection-a',
    connection,
    clientServerWire.decodeClientServerControl(clientServerWire.encodeClientServerUpdate({
    ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
    descriptorRevision: 2n,
    weight: 50,
    securityIdentity: 'cluster-a'
    }, 1024)).admission
  ), error => error.name === 'ServiceWireProtocolError' && /conflicts/.test(error.message));
  assert.equal(sockets.clientServerActiveTargets('orders')[0].weight, 25);
  await sockets.dispose();
});

test('ClientServer reserved hello is consumed before application dispatch and returns exact admit', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        server: { bind: 'tcp://127.0.0.1:9401' },
        sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }]
      }
    }
  });
  const sockets = new ZLinkChannelSocketRegistry(registration, {}, {});
  const server = {
    ...descriptor({
      token: { ownerId: 'server-owner', leaseGeneration: 5n }
    }),
    updatedAt: new Date()
  };
  sockets.setClientServerServerDescriptor(server, 'orders');
  const hello = zlink.Message.from(clientServerWire.encodeClientServerHello({
    channelName: 'orders',
    securityIdentity: 'cluster-a',
    normalizedEffectiveMaxMessageBytes: 1024
  }));
  let reply;
  let pushed;
  let received = {
    parts: [hello],
    replyToken: {},
    routingId: 'client-a',
    close() { hello.close(); }
  };
  const router = {
    maxMessageSize: 4096,
    recv() {
      const value = received;
      received = undefined;
      return value;
    },
    reply(_routingId, _replyToken, message) {
      reply = Buffer.from(message.data());
    },
    async send(_routingId, message) {
      pushed = Buffer.from(message.data());
    },
    async dispose() {}
  };
  let applicationDispatches = 0;
  const loop = new ZLinkChannelReceiveLoop(
    'orders',
    router,
    { async dispatch() { applicationDispatches++; } },
    undefined,
    (record, socket) =>
      sockets.tryHandleClientServerControl('orders', record, socket),
    readyPoller(),
    new ApplicationJobQueue(resolveApplicationJobQueueConfiguration())
  );
  const controller = new AbortController();
  const running = loop.run(controller.signal);
  await new Promise(resolve => setImmediate(resolve));
  controller.abort();
  await loop.stop();
  await running;

  assert.equal(applicationDispatches, 0);
  const decoded = clientServerWire.decodeClientServerControl(reply);
  assert.equal(decoded.kind, 'admit');
  assert.equal(decoded.admission.serverRid, String(server.serverRid));
  assert.equal(decoded.admission.lifecycleGeneration, server.lifecycleGeneration);
  assert.equal(decoded.admission.securityIdentity, server.securityIdentity);
  assert.equal(decoded.admission.normalizedEffectiveMaxMessageBytes, 1024);
  sockets.channelRouters.set('orders', router);
  sockets.setClientServerServerDescriptor({
    ...server,
    descriptorRevision: 2n
  }, 'orders');
  await new Promise(resolve => setImmediate(resolve));
  const update = clientServerWire.decodeClientServerControl(pushed);
  assert.equal(update.kind, 'update');
  assert.equal(update.admission.normalizedEffectiveMaxMessageBytes, 1024);
  await sockets.dispose();
});

test('ClientServer server probes each admitted client and fences ACK by routing identity', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        server: { bind: 'tcp://127.0.0.1:9401' },
        sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }]
      }
    }
  });
  const sent = [];
  const disconnected = [];
  const diagnostics = [];
  const router = {
    nativeInstance: {},
    lastEndpoint: 'tcp://127.0.0.1:9401',
    peerWeight: 100,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: -1,
    maxMessageSize: 4096,
    setChannelName() {},
    setRoutingId() {},
    bind() {},
    async send(routingId, message) {
      sent.push({ routingId, frame: Buffer.from(message.data()) });
    },
    disconnectPeer(routingId) { disconnected.push(routingId); },
    reply() {},
    async dispose() {}
  };
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createRouterSocket() { return router; },
      createReadablePoller() { return readyPoller(); }
    },
    {},
    undefined,
    error => diagnostics.push(error)
  );
  sockets.clientServerServerIdentity('orders');
  const server = {
    ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
    securityIdentity: 'default'
  };
  sockets.setClientServerServerDescriptor(server, 'orders');
  const hello = zlink.Message.from(clientServerWire.encodeClientServerHello({
    channelName: 'orders',
    securityIdentity: 'default',
    normalizedEffectiveMaxMessageBytes: 4096
  }));
  sockets.tryHandleClientServerControl('orders', {
    parts: [hello],
    replyToken: {},
    routingId: 'client-a'
  }, router);
  hello.close();

  const base = performance.now();
  await sockets.tickClientServerLiveness(base + 5_001);
  const probe = clientServerWire.decodeClientServerControl(sent.at(-1).frame);
  assert.equal(probe.kind, 'livenessProbe');
  await sockets.tickClientServerLiveness(base + 10_002);
  const retransmit = clientServerWire.decodeClientServerControl(sent.at(-1).frame);
  assert.equal(retransmit.kind, 'livenessProbe');
  assert.equal(retransmit.probeId, probe.probeId);
  const wrongAck = zlink.Message.from(
    clientServerWire.encodeClientServerLivenessAck(probe.probeId)
  );
  assert.equal(sockets.tryHandleClientServerControl('orders', {
    parts: [wrongAck],
    replyToken: null,
    routingId: 'client-b'
  }, router), true);
  wrongAck.close();
  assert.equal(diagnostics.length, 1);
  assert.match(diagnostics[0].message, /stale or duplicate liveness ACK/);
  await sockets.tickClientServerLiveness(base + 15_001);
  assert.deepEqual(disconnected, ['client-a']);

  const beforeUpdate = sent.length;
  sockets.setClientServerServerDescriptor({
    ...server,
    descriptorRevision: 2n,
    weight: 25
  }, 'orders');
  assert.equal(sent.length, beforeUpdate);
  await sockets.dispose();
});

test('ClientServer server publishes its concrete endpoint then drains and removes its descriptor', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const runtime = new internal.ZLinkLocationRuntime({
    stores: stores(store),
    ownerId: 'local-owner'
  });
  await runtime.start('host-a');
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        server: { bind: 'tcp://0.0.0.0:0', advertiseHost: 'orders.internal', weight: 75 },
        sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }]
      }
    },
    locations: { useInMemoryStores: true }
  });
  const dealerCalls = [];
  const sockets = {
    serverWeight: 75,
    clientServerServerIdentity() {
      return {
        serverRid: 'server-a',
        lifecycleGeneration: 11n,
        endpoint: 'tcp://orders.internal:49152'
      };
    },
    clientDealer() {
      return {
        connect(endpoint) { dealerCalls.push(`connect:${endpoint}`); },
        disconnect(endpoint) { dealerCalls.push(`disconnect:${endpoint}`); }
      };
    },
    clientServerServerWeight() { return this.serverWeight; },
    setClientServerServerDescriptor() {}
  };
  const discovery = new internal.ZLinkClientServerLocationRuntime(
    registration,
    sockets,
    runtime,
    stores(store),
    { pollingIntervalMs: 60_000 }
  );

  await discovery.start();
  const published = await store.listClientServers('orders');
  assert.equal(published.items.length, 1);
  assert.equal(published.items[0].endpoint, 'tcp://orders.internal:49152');
  assert.equal(published.items[0].serverRid, 'server-a');
  assert.equal(published.items[0].lifecycleGeneration, 11n);
  assert.deepEqual(dealerCalls, []);

  sockets.serverWeight = 25;
  await discovery.tick();
  const reweighted = await store.listClientServers('orders');
  assert.equal(reweighted.items[0].weight, 25);
  assert.equal(reweighted.items[0].descriptorRevision, 2n);
  await discovery.tick();
  assert.equal((await store.listClientServers('orders')).items[0].descriptorRevision, 2n);

  await discovery.stop();
  assert.equal((await store.listClientServers('orders')).items.length, 0);
  await runtime.stop();
});

test('same-process ClientServer Server is discovered through the normal DEALER transport', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const runtime = new internal.ZLinkLocationRuntime({
    stores: stores(store),
    ownerId: 'local-owner'
  });
  await runtime.start('local-host');
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        client: { manualConnections: [] },
        server: { bind: 'tcp://0.0.0.0:0', advertiseHost: '127.0.0.1', weight: 75 },
        sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }]
      }
    },
    locations: { useInMemoryStores: true }
  });
  const sockets = automaticClientServerSockets();
  sockets.clientServerServerIdentity = () => ({
    serverRid: 'local-server',
    lifecycleGeneration: 11n,
    endpoint: 'tcp://127.0.0.1:49152'
  });
  sockets.clientServerServerSocket = () => ({ peerWeight: 75 });
  const discovery = new internal.ZLinkClientServerLocationRuntime(
    registration,
    sockets,
    runtime,
    stores(store),
    { pollingIntervalMs: 60_000 }
  );

  await discovery.start();
  assert.deepEqual(sockets.calls, ['connect:tcp://127.0.0.1:49152']);
  const published = (await store.listClientServers('orders')).items[0];
  assert.equal(published.ownerId, 'local-owner');
  await sockets.admit(published);
  assert.equal(discovery.activeTargets('orders')[0].serverRid, 'local-server');

  await discovery.stop();
  await runtime.stop();
});

test('automatic ClientServer client reconciles dedicated server descriptors by RID and lifecycle', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const localRuntime = new internal.ZLinkLocationRuntime({
    stores: stores(store),
    ownerId: 'client-owner'
  });
  await localRuntime.start('client-host');
  const remoteOwner = await store.claimOwnerLease('remote-owner', 30_000);
  assert.equal(remoteOwner.kind, 'claimed');
  await store.updateClientServer(
    descriptor(remoteOwner),
    internal.ZLinkLocationWriteIntent.NewClaim
  );

  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const sockets = automaticClientServerSockets();
  const discovery = new internal.ZLinkClientServerLocationRuntime(
    registration,
    sockets,
    localRuntime,
    stores(store),
    { pollingIntervalMs: 60_000 }
  );

  await discovery.start();
  assert.deepEqual(sockets.calls, ['connect:tcp://10.0.0.1:9401']);
  assert.equal(discovery.activeTargets('orders').length, 0);
  await sockets.admit(descriptor(remoteOwner));
  assert.equal(discovery.activeTargets('orders')[0].lifecycleGeneration, 7n);

  await store.updateClientServer(
    descriptor(remoteOwner, {
      descriptorRevision: 2n,
      endpoint: 'tcp://10.0.0.2:9401'
    }),
    internal.ZLinkLocationWriteIntent.Renew
  );
  // Endpoint is immutable in one lifecycle, so the store fences the mutation.
  await discovery.tick();
  assert.deepEqual(sockets.calls, ['connect:tcp://10.0.0.1:9401']);

  await store.removeClientServer(
    { channelName: 'orders', serverRid: 'server-a' },
    remoteOwner.token
  );
  await discovery.tick();
  assert.deepEqual(sockets.calls, [
    'connect:tcp://10.0.0.1:9401',
    'disconnect:tcp://10.0.0.1:9401'
  ]);

  await discovery.stop();
  await localRuntime.stop();
});

test('same ClientServer RID and endpoint reset transport readiness on a new lifecycle', async () => {
  let nowMs = Date.UTC(2026, 6, 23, 0, 0, 0);
  const store = new internal.ZLinkInMemoryLocationStore(() => new Date(nowMs));
  const localRuntime = new internal.ZLinkLocationRuntime({
    stores: stores(store),
    ownerId: 'client-owner'
  });
  await localRuntime.start('client-host');
  const oldOwner = await store.claimOwnerLease('old-server-owner', 100);
  assert.equal(oldOwner.kind, 'claimed');
  await store.updateClientServer(
    descriptor(oldOwner),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const sockets = automaticClientServerSockets();
  const discovery = new internal.ZLinkClientServerLocationRuntime(
    registration,
    sockets,
    localRuntime,
    stores(store),
    { pollingIntervalMs: 60_000 }
  );
  await discovery.start();
  await sockets.admit(descriptor(oldOwner));

  nowMs += 101;
  const newOwner = await store.claimOwnerLease('new-server-owner', 30_000);
  assert.equal(newOwner.kind, 'claimed');
  const takeover = await store.updateClientServer(
    descriptor(newOwner, { lifecycleGeneration: 8n }),
    internal.ZLinkLocationWriteIntent.Takeover
  );
  assert.equal(takeover.status, internal.ZLinkLocationWriteStatus.Stored);
  await discovery.tick();

  assert.deepEqual(sockets.calls, [
    'connect:tcp://10.0.0.1:9401',
    'connect:tcp://10.0.0.1:9401'
  ]);
  assert.equal(discovery.activeTargets('orders')[0].lifecycleGeneration, 7n);
  await sockets.admit(descriptor(newOwner, { lifecycleGeneration: 8n }));
  assert.deepEqual(sockets.calls, [
    'connect:tcp://10.0.0.1:9401',
    'connect:tcp://10.0.0.1:9401',
    'disconnect:tcp://10.0.0.1:9401'
  ]);
  assert.equal(discovery.activeTargets('orders')[0].lifecycleGeneration, 8n);
  sockets.terminate(7n);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(discovery.activeTargets('orders')[0].lifecycleGeneration, 8n);
  await discovery.stop();
  await localRuntime.stop();
});

test('automatic ClientServer retains intent across close and fences obsolete admission results', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const localRuntime = new internal.ZLinkLocationRuntime({
    stores: stores(store), ownerId: 'client-owner'
  });
  await localRuntime.start('client-host');
  const remoteOwner = await store.claimOwnerLease('remote-owner', 30_000);
  const expected = descriptor(remoteOwner);
  await store.updateClientServer(expected, internal.ZLinkLocationWriteIntent.NewClaim);
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const sockets = automaticClientServerSockets();
  const discovery = new internal.ZLinkClientServerLocationRuntime(
    registration, sockets, localRuntime, stores(store), { pollingIntervalMs: 60_000 }
  );
  try {
    await discovery.start();
    await sockets.admit(expected);
    const connection = [...sockets.connections.values()][0];
    sockets.terminate(7n);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(discovery.activeTargets('orders').length, 0);
    await discovery.tick();
    assert.deepEqual(sockets.calls, [`connect:${expected.endpoint}`]);

    connection.callbacks.onTransportReady(expected.serverRid, expected.endpoint);
    await new Promise(resolve => setImmediate(resolve));
    const obsoleteReply = connection.reply;
    sockets.terminate(7n);
    connection.callbacks.onTransportReady(expected.serverRid, expected.endpoint);
    await new Promise(resolve => setImmediate(resolve));
    assert.notEqual(connection.reply, obsoleteReply);
    obsoleteReply([zlink.Message.from(clientServerWire.encodeClientServerAdmit(expected, 4096))]);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(discovery.activeTargets('orders').length, 0);
    connection.reject(new Error('transport request failed'));
    await new Promise(resolve => setImmediate(resolve));
    assert.match((await localRuntime.getStatus()).lastError, /transport request failed/);
    await discovery.tick();
    assert.deepEqual(sockets.calls, [`connect:${expected.endpoint}`]);

    sockets.terminate(7n);
    await sockets.admit(expected);
    assert.equal(discovery.activeTargets('orders').length, 1);
    assert.deepEqual(sockets.calls, [`connect:${expected.endpoint}`]);
    assert.equal(sockets.connections.size, 1);
  } finally {
    await discovery.stop();
    await localRuntime.stop();
  }
});

test('ClientServer target remains unavailable when service admission mismatches security identity', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const localRuntime = new internal.ZLinkLocationRuntime({
    stores: stores(store),
    ownerId: 'client-owner'
  });
  await localRuntime.start('client-host');
  const remoteOwner = await store.claimOwnerLease('remote-owner', 30_000);
  assert.equal(remoteOwner.kind, 'claimed');
  const expected = descriptor(remoteOwner);
  await store.updateClientServer(expected, internal.ZLinkLocationWriteIntent.NewClaim);
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const sockets = automaticClientServerSockets();
  const discovery = new internal.ZLinkClientServerLocationRuntime(
    registration,
    sockets,
    localRuntime,
    stores(store),
    { pollingIntervalMs: 60_000 }
  );
  await discovery.start();
  await sockets.admit(expected, { securityIdentity: 'wrong-cluster' });

  assert.equal(discovery.activeTargets('orders').length, 0);
  assert.deepEqual(sockets.calls, [
    'connect:tcp://10.0.0.1:9401',
    'disconnect:tcp://10.0.0.1:9401'
  ]);
  assert.match((await localRuntime.getStatus()).lastError, /admission does not match/);
  await discovery.tick();
  assert.deepEqual(sockets.calls, [
    'connect:tcp://10.0.0.1:9401',
    'disconnect:tcp://10.0.0.1:9401'
  ]);
  await discovery.stop();
  await localRuntime.stop();
});

test('periodic ClientServer discovery failures remain observable on location runtime status', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const runtime = new internal.ZLinkLocationRuntime({
    stores: stores(store),
    ownerId: 'client-owner'
  });
  await runtime.start('client-host');
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [] } } },
    locations: { useInMemoryStores: true }
  });
  const dealer = { connect() {}, disconnect() {} };
  const discovery = new internal.ZLinkClientServerLocationRuntime(
    registration,
    { clientDealer() { return dealer; } },
    runtime,
    stores(store),
    { pollingIntervalMs: 1 }
  );
  await discovery.start();
  const originalList = store.listClientServers.bind(store);
  store.listClientServers = async () => {
    throw new Error('client-server-store-unavailable');
  };
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.match((await runtime.getStatus()).lastError, /client-server-store-unavailable/);
  store.listClientServers = originalList;
  await discovery.stop();
  await runtime.stop();
});

function readyWaitSockets(requestTimeoutMs) {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        client: { manualConnections: [] },
        requestTimeoutMs
      }
    },
    locations: { useInMemoryStores: true }
  });
  const dealer = fakeDealer('dealer-0');
  const created = [];
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() {
        created.push(dealer);
        return dealer;
      },
      createReadablePoller() { return readyPoller(); }
    },
    {},
    {
      openSocketMonitor() {
        return { nativeInstance: {}, onEvent() {}, drain() { return 0; }, async dispose() {} };
      }
    }
  );
  sockets.openClientServerConnection(
    'orders',
    'orders-a:7',
    'tcp://10.0.0.1:9401',
    { onTransportReady() {}, onTerminated() {} }
  );
  return { sockets, dealer, created, registration };
}

function readyWaitClient(t, channelTimeoutMs = 60_000) {
  const fixture = readyWaitSockets(channelTimeoutMs);
  const outbound = new ZLinkChannelOutboundOperations(
    fixture.sockets,
    { serializers: fixture.registration.messageSerializers },
    new ZLinkChannelDispatchServices(fixture.registration)
  );
  const client = new internal.DefaultZLinkChannelClient(fixture.registration, outbound);
  class Lookup { id = 1; }
  t.after(() => fixture.sockets.dispose());
  return {
    ...fixture,
    request: timeoutMs => client.requestToChannel('orders', new Lookup()).timeout(timeoutMs).submit(t.signal),
    admit: () => fixture.sockets.admitClientServerConnection(
      discoveryDescriptor('server-a', 100), 'orders-a:7'
    )
  };
}

for (const knownTarget of [false, true]) {
  test(`ClientServer call deadline bounds a short timeout with ${knownTarget ? 'a weight-zero' : 'no known'} server`, async (t) => {
    const { request, dealer, sockets } = readyWaitClient(t);
    if (knownTarget) {
      sockets.admitClientServerConnection(discoveryDescriptor('server-a', 0), 'orders-a:7');
    }
    assert.equal(sockets.hasKnownClientServerTargets('orders'), knownTarget);
    assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
    const submitted = t.mock.method(dealer, 'request');
    const startedAt = performance.now();
    await assert.rejects(request(200), {
      kind: knownTarget ? framework.ZLinkFrameworkErrorKind.Unavailable : framework.ZLinkFrameworkErrorKind.NotFound
    });
    const elapsed = performance.now() - startedAt;
    // The extra 50 ms permits event-loop scheduling after the 200 ms deadline, not another wait budget.
    assert.ok(elapsed >= 200 && elapsed < 250, `200 ms call finished after ${elapsed} ms`);
    assert.equal(submitted.mock.callCount(), 0);
  });
}

test('ClientServer call deadline gives a server admitted after 100 ms only the remaining time', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  let nowMs = 0;
  t.mock.method(performance, 'now', () => nowMs);
  // A shorter Channel default must not truncate the explicit 200 ms call either.
  const { request, dealer, admit } = readyWaitClient(t, 50);
  let submittedAfterMs;
  let submittedTimeoutMs;
  let admittedAfterMs;
  const startedAt = performance.now();
  dealer.request = async (parts, timeoutMs) => {
    assert.equal(Number.isInteger(timeoutMs), true, 'binding request timeout must be integer milliseconds');
    submittedAfterMs = performance.now() - startedAt;
    submittedTimeoutMs = timeoutMs;
    const messages = parts.map(part => zlink.Message.from(part));
    try {
      const header = channelEnvelope.decodeChannelHeader(messages);
      return channelEnvelope.encodeChannelReplyParts(header, { found: true })
        .map(part => zlink.Message.from(part));
    } finally {
      channelEnvelope.closeMessages(messages);
    }
  };
  const admission = setTimeout(() => {
    admittedAfterMs = performance.now() - startedAt;
    admit();
  }, 100);
  t.after(() => clearTimeout(admission));
  const pending = request(200);
  nowMs = 100;
  t.mock.timers.tick(100);
  assert.deepEqual(await pending, { found: true });
  assert.equal(admittedAfterMs, 100);
  assert.equal(submittedTimeoutMs, 100);
  assert.equal(submittedAfterMs + submittedTimeoutMs, 200);
});

test('ClientServer call deadline still expires at submission plus timeout after late admission', async (t) => {
  const { request, dealer, admit } = readyWaitClient(t);
  let attempts = 0;
  dealer.request = async (_parts, timeoutMs) => {
    assert.equal(Number.isInteger(timeoutMs), true, 'binding request timeout must be integer milliseconds');
    attempts++;
    await new Promise(resolve => setTimeout(resolve, timeoutMs));
    throw new ZLinkBackendResultError('request', RequestResult.TimedOut);
  };
  const admission = setTimeout(admit, 100);
  t.after(() => clearTimeout(admission));
  const startedAt = performance.now();
  await assert.rejects(request(200), { kind: framework.ZLinkFrameworkErrorKind.DeadlineExceeded });
  const elapsed = performance.now() - startedAt;
  assert.ok(elapsed >= 195 && elapsed < 250, `200 ms call finished after ${elapsed} ms`);
  assert.equal(attempts, 1);
});

test('ClientServer call deadline rejects a target selected after the deadline without submitting', async (t) => {
  const { request, dealer, sockets, admit } = readyWaitClient(t);
  let nowMs = 0;
  t.mock.method(performance, 'now', () => nowMs);
  const select = sockets.clientDealerForOutbound.bind(sockets);
  t.mock.method(sockets, 'clientDealerForOutbound', channelName => {
    const selected = select(channelName);
    nowMs = 200;
    return selected;
  });
  const submitted = t.mock.method(dealer, 'request');
  admit();
  await assert.rejects(request(200), { kind: framework.ZLinkFrameworkErrorKind.Unavailable });
  assert.equal(performance.now(), 200);
  assert.equal(submitted.mock.callCount(), 0);
});

test('ClientServer call deadline caps a long per-call ready wait at five seconds', { timeout: 7_000 }, async (t) => {
  const { request, dealer } = readyWaitClient(t, 50);
  const submitted = t.mock.method(dealer, 'request');
  const startedAt = performance.now();
  await assert.rejects(request(60_000), { kind: framework.ZLinkFrameworkErrorKind.NotFound });
  const elapsed = performance.now() - startedAt;
  assert.ok(elapsed >= 5_000 && elapsed < 5_500, `5 s ready cap finished after ${elapsed} ms`);
  assert.equal(submitted.mock.callCount(), 0);
});

test('ClientServer send target waits for admission already in flight instead of failing at once', async () => {
  const { sockets, dealer, created } = readyWaitSockets(60_000);
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
  assert.equal(created.length, 1);

  const startedAt = performance.now();
  let admittedAfterMs;
  const admission = setTimeout(() => {
    admittedAfterMs = performance.now() - startedAt;
    sockets.admitClientServerConnection(discoveryDescriptor('server-a', 100), 'orders-a:7');
  }, 40);
  const selected = await sockets.awaitClientDealerForOutbound('orders');
  clearTimeout(admission);
  const elapsed = performance.now() - startedAt;

  assert.equal(selected, dealer);
  // A wait that did not yield to the event loop would starve this timer, which stands in for the
  // monitor callbacks that carry real admission, and would fail after burning the whole bound.
  assert.equal(typeof admittedAfterMs, 'number');
  assert.ok(elapsed >= 40, `expected the wait to span admission, waited ${elapsed}ms`);
  assert.ok(elapsed < 5_000, `expected admission to end the wait early, waited ${elapsed}ms`);
  // The wait observes admission already in flight and never starts one, so it opens no connection.
  assert.equal(created.length, 1);
  await sockets.dispose();
});

test('ClientServer send target waits the Channel request timeout when only a weight 0 candidate exists', async () => {
  const { sockets } = readyWaitSockets(120);
  sockets.admitClientServerConnection(discoveryDescriptor('server-a', 0), 'orders-a:7');
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);

  const startedAt = performance.now();
  const selected = await sockets.awaitClientDealerForOutbound('orders');
  const elapsed = performance.now() - startedAt;

  assert.equal(selected, undefined);
  assert.ok(elapsed >= 120, `expected the configured request timeout to bound the wait, waited ${elapsed}ms`);
  assert.ok(elapsed < 5_000, `expected the shorter request timeout to win over the 5s cap, waited ${elapsed}ms`);
  await sockets.dispose();
});

test('ClientServer send target caps the readiness wait at five seconds', async () => {
  const { sockets } = readyWaitSockets(60_000);

  const startedAt = performance.now();
  const selected = await sockets.awaitClientDealerForOutbound('orders');
  const elapsed = performance.now() - startedAt;

  assert.equal(selected, undefined);
  assert.ok(elapsed >= 5_000, `expected the five second cap to bound the wait, waited ${elapsed}ms`);
  assert.ok(elapsed < 8_000, `expected the cap to end the wait, waited ${elapsed}ms`);
  await sockets.dispose();
});

for (const [direction, jumpMs] of [['forward', 10_000], ['backward', -10_000]]) {
  test(`ClientServer readiness cap survives a wall-clock jump ${direction}`, { timeout: 10_000 }, async (t) => {
    const { sockets } = readyWaitSockets(60_000);
    const wallNow = Date.now.bind(Date);
    let offsetMs = 0;
    t.mock.method(Date, 'now', () => wallNow() + offsetMs);
    const jump = setTimeout(() => { offsetMs = jumpMs; }, 40);
    t.after(async () => {
      clearTimeout(jump);
      await sockets.dispose();
    });

    const startedAt = performance.now();
    const selected = await sockets.awaitClientDealerForOutbound('orders', t.signal);
    const elapsed = performance.now() - startedAt;

    assert.equal(offsetMs, jumpMs);
    assert.equal(selected, undefined);
    assert.ok(elapsed >= 5_000, `expected the five second cap to bound the wait, waited ${elapsed}ms`);
    assert.ok(elapsed < 8_000, `expected the cap to end the wait, waited ${elapsed}ms`);
  });
}

test('ClientServer outbound reports no selectable target as RequestTargetNotFound', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        client: { manualConnections: [] },
        requestTimeoutMs: 60
      }
    },
    locations: { useInMemoryStores: true }
  });
  const manager = new internal.ZLinkChannelRuntimeManager(
    registration,
    {},
    { nativeInstance: {}, shutdown() {}, async dispose() {} }
  );

  assert.deepEqual(
    await manager.send('orders', 'Notice', { id: 1 }),
    { status: submissionResult.ZLinkSubmitStatus.TargetNotFound }
  );
  await assert.rejects(
    () => manager.request('orders', 'Lookup', { id: 1 }, 60),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
      // The kind carries retry policy; RequestTargetNotFound is not retriable by default in any
      // lane, and the reference throws omit the flag exactly as this one does.
      && !('isRetriable' in error)
  );
  await manager.dispose();
});

test('ClientServer outbound reports a missing Client role as NotConfigured', async () => {
  const registration = internal.createFrameworkRegistration({
    channels: {
      orders: {
        server: { bind: 'inproc://orders' },
        sendHandlers: [{ packetName: 'Notice', handler: { handle() {} } }]
      }
    }
  });
  const manager = new internal.ZLinkChannelRuntimeManager(
    registration,
    {},
    { nativeInstance: {}, shutdown() {}, async dispose() {} }
  );

  await assert.rejects(
    () => manager.send('orders', 'Notice', { id: 1 }),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotConfigured
  );
  await assert.rejects(
    () => manager.request('orders', 'Lookup', { id: 1 }, 60),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotConfigured
  );
  await manager.dispose();
});

function automaticClientServerSockets() {
  const connections = new Map();
  const history = new Map();
  const ready = new Map();
  const calls = [];
  return {
    calls,
    connections,
    openClientServerConnection(channelName, connectionId, endpoint, callbacks) {
      const dealer = {
        maxMessageSize: 0x7fff_ffff,
        request(message) {
          const connection = connections.get(connectionId);
          connection.hello = Buffer.from(message.data());
          return new Promise((resolve, reject) => {
            connection.reply = resolve;
            connection.reject = reject;
          });
        }
      };
      connections.set(connectionId, {
        channelName,
        connectionId,
        endpoint,
        callbacks,
        dealer
      });
      history.set(connectionId, connections.get(connectionId));
      calls.push(`connect:${endpoint}`);
      return dealer;
    },
    async closeClientServerConnection(connectionId) {
      const connection = connections.get(connectionId);
      if (connection === undefined) return;
      connections.delete(connectionId);
      ready.delete(connectionId);
      calls.push(`disconnect:${connection.endpoint}`);
    },
    admitClientServerConnection(value, connectionId) {
      if (!connections.has(connectionId)) return false;
      ready.set(connectionId, value);
      return true;
    },
    removeClientServerReady(_channelName, _serverRoutingId, connectionId) {
      return ready.delete(connectionId);
    },
    setClientServerServerDescriptor() {},
    clientServerServerSocket() { return { peerWeight: 100 }; },
    async admit(value, overrides = {}) {
      const connectionId = [...connections.keys()]
        .find(id => id.endsWith(`:${value.lifecycleGeneration}`));
      assert.notEqual(connectionId, undefined);
      const connection = connections.get(connectionId);
      connection.callbacks.onTransportReady(String(value.serverRid), value.endpoint);
      await new Promise(resolve => setImmediate(resolve));
      assert.notEqual(connection.reply, undefined);
      const reply = zlink.Message.from(clientServerWire.encodeClientServerAdmit({
        ...value,
        ...overrides
      }, 0x7fff_ffff));
      connection.reply([reply]);
      await new Promise(resolve => setImmediate(resolve));
    },
    terminate(lifecycleGeneration) {
      const connectionId = [...history.keys()]
        .find(id => id.endsWith(`:${lifecycleGeneration}`));
      const connection = connectionId === undefined ? undefined : history.get(connectionId);
      connection?.callbacks.onTerminated(String(connection.serverRid), connection.endpoint);
    }
  };
}

function fakeDealer(id) {
  return {
    id,
    nativeInstance: {},
    peerWeight: 100,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: -1,
    maxMessageSize: -1,
    setChannelName() {},
    setRoutingId() {},
    connect() {},
    disconnect() {},
    async send() {},
    async request() { return []; },
    recv() { return undefined; },
    async dispose() {}
  };
}

function readyPoller() {
  return {
    wait() { return true; },
    dispose() {}
  };
}

function discoveryDescriptor(serverRoutingId, weight) {
  return {
    channelName: 'orders',
    serverRoutingId,
    lifecycleGeneration: 7n,
    descriptorRevision: 1n,
    weight,
    state: 'serving',
    securityIdentity: 'cluster-a',
    effectiveMaxMessageBytes: 1024,
    advertisedEndpoint: `tcp://10.0.0.${serverRoutingId.endsWith('a') ? 1 : 2}:9401`
  };
}

function receivedControl(frame) {
  const message = zlink.Message.from(frame);
  return {
    parts: [message],
    close() { message.close(); }
  };
}
