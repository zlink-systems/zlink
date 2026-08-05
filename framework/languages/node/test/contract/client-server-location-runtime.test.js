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
const clientServerWire = require(
  '../../packages/framework/dist/runtime/channels/client-server-service-wire'
);
const submissionResult = require(
  '../../packages/framework/dist/runtime/messaging/submission-result'
);

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
    onSendReady() {},
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
    dealer.request = (message, callback) => {
      requests.push({ frame: Buffer.from(message.data()), callback });
      return true;
    };
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
    local.requests[0].callback(0, [
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
          return { nativeInstance: {}, onEvent() {}, async dispose() {} };
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
  let monitorHandler;
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() {
        const dealer = fakeDealer(`manual-${dealers.length}`);
        dealer.requests = [];
        dealer.connect = value => { dealer.connected = value; };
        dealer.request = (message, callback) => {
          dealer.requests.push({ frame: Buffer.from(message.data()), callback });
          return true;
        };
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
  dealers[0].requests[0].callback(0, [
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
  await sockets.dispose();
});

test('manual ClientServer reconnect fences a late admission from the previous physical pipe', async () => {
  const endpoint = 'tcp://10.0.0.1:9401';
  const registration = internal.createFrameworkRegistration({
    channels: { orders: { client: { manualConnections: [endpoint] } } }
  });
  const dealer = fakeDealer('manual-race');
  const requests = [];
  dealer.request = (message, callback) => {
    requests.push({ frame: Buffer.from(message.data()), callback });
    return true;
  };
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
  requests[0].callback(0, [
    zlink.Message.from(clientServerWire.encodeClientServerAdmit(admitted, 4096))
  ]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);

  requests[1].callback(0, [
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
  dealer.recv = () => inbound.shift();
  dealer.send = message => {
    sent.push(Buffer.from(message.data()));
    return true;
  };
  dealer.request = (message, callback) => {
    requests.push({ frame: Buffer.from(message.data()), callback });
    return true;
  };
  const sockets = new ZLinkChannelSocketRegistry(
    registration,
    {
      createDealerSocket() { return dealer; },
      createReadablePoller() { return readyPoller(); }
    },
    {},
    {
      openSocketMonitor() {
        return { nativeInstance: {}, onEvent() {}, async dispose() {} };
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

  const base = performance.now();
  inbound.push(receivedControl(clientServerWire.encodeClientServerLivenessProbe(91n)));
  sockets.tickClientServerLiveness(base);
  const serverProbeAck = clientServerWire.decodeClientServerControl(sent[0]);
  assert.equal(serverProbeAck.kind, 'livenessAck');
  assert.equal(serverProbeAck.probeId, 91n);
  sockets.tickClientServerLiveness(base + 5_001);
  const probe = clientServerWire.decodeClientServerControl(requests[0].frame);
  assert.equal(probe.kind, 'livenessProbe');
  sockets.tickClientServerLiveness(base + 10_002);
  const retransmit = clientServerWire.decodeClientServerControl(requests[1].frame);
  assert.equal(retransmit.kind, 'livenessProbe');
  assert.equal(retransmit.probeId, probe.probeId);
  requests[0].callback(0, [
    zlink.Message.from(clientServerWire.encodeClientServerLivenessAck(probe.probeId + 1n))
  ]);
  sockets.tickClientServerLiveness(base + 15_001);
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
  await sockets.dispose();
});

test('ClientServer pushed descriptor updates accept only current higher revision', async () => {
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
        return { nativeInstance: {}, onEvent() {}, async dispose() {} };
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
  sockets.tickClientServerLiveness();
  assert.equal(sockets.clientServerActiveTargets('orders')[0].weight, 25);

  inbound.push(receivedControl(clientServerWire.encodeClientServerUpdate({
    ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
    descriptorRevision: 1n,
    weight: 100,
    securityIdentity: 'cluster-a'
  }, 1024)));
  sockets.tickClientServerLiveness();
  assert.equal(sockets.clientServerActiveTargets('orders')[0].weight, 25);

  inbound.push(receivedControl(clientServerWire.encodeClientServerUpdate({
    ...descriptor({ token: { ownerId: 'owner', leaseGeneration: 1n } }),
    descriptorRevision: 2n,
    weight: 50,
    securityIdentity: 'cluster-a'
  }, 1024)));
  sockets.tickClientServerLiveness();
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
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
  let received = {
    parts: [hello],
    requestSeq: 1n,
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
    reply(_routingId, _requestSeq, message) {
      reply = Buffer.from(message.data());
    }
  };
  let applicationDispatches = 0;
  const loop = new ZLinkChannelReceiveLoop(
    'orders',
    router,
    { async dispatch() { applicationDispatches++; } },
    undefined,
    (record, socket) =>
      sockets.tryHandleClientServerControl('orders', record, socket),
    undefined,
    readyPoller()
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
  assert.equal(decoded.admission.normalizedEffectiveMaxMessageBytes, 4096);
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
  let acceptSend = true;
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
    onSendReady() {},
    bind() {},
    send(routingId, message) {
      sent.push({ routingId, frame: Buffer.from(message.data()) });
      return acceptSend;
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
    {}
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
    requestSeq: 1n,
    routingId: 'client-a'
  }, router);
  hello.close();

  const base = performance.now();
  sockets.tickClientServerLiveness(base + 5_001);
  const probe = clientServerWire.decodeClientServerControl(sent.at(-1).frame);
  assert.equal(probe.kind, 'livenessProbe');
  acceptSend = false;
  sockets.tickClientServerLiveness(base + 10_002);
  const retransmit = clientServerWire.decodeClientServerControl(sent.at(-1).frame);
  assert.equal(retransmit.kind, 'livenessProbe');
  assert.equal(retransmit.probeId, probe.probeId);
  const wrongAck = zlink.Message.from(
    clientServerWire.encodeClientServerLivenessAck(probe.probeId)
  );
  assert.equal(sockets.tryHandleClientServerControl('orders', {
    parts: [wrongAck],
    requestSeq: null,
    routingId: 'client-b'
  }, router), true);
  wrongAck.close();
  sockets.tickClientServerLiveness(base + 15_001);
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
        return { nativeInstance: {}, onEvent() {}, async dispose() {} };
      }
    }
  );
  sockets.openClientServerConnection(
    'orders',
    'orders-a:7',
    'tcp://10.0.0.1:9401',
    { onTransportReady() {}, onTerminated() {} }
  );
  return { sockets, dealer, created };
}

test('ClientServer send target waits for admission already in flight instead of failing at once', async () => {
  const { sockets, dealer, created } = readyWaitSockets(60_000);
  assert.equal(sockets.clientDealerForOutbound('orders'), undefined);
  assert.equal(created.length, 1);

  const startedAt = Date.now();
  let admittedAfterMs;
  const admission = setTimeout(() => {
    admittedAfterMs = Date.now() - startedAt;
    sockets.admitClientServerConnection(discoveryDescriptor('server-a', 100), 'orders-a:7');
  }, 40);
  const selected = await sockets.awaitClientDealerForOutbound('orders');
  clearTimeout(admission);
  const elapsed = Date.now() - startedAt;

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

  const startedAt = Date.now();
  const selected = await sockets.awaitClientDealerForOutbound('orders');
  const elapsed = Date.now() - startedAt;

  assert.equal(selected, undefined);
  assert.ok(elapsed >= 120, `expected the configured request timeout to bound the wait, waited ${elapsed}ms`);
  assert.ok(elapsed < 5_000, `expected the shorter request timeout to win over the 5s cap, waited ${elapsed}ms`);
  await sockets.dispose();
});

test('ClientServer send target caps the readiness wait at five seconds', async () => {
  const { sockets } = readyWaitSockets(60_000);

  const startedAt = Date.now();
  const selected = await sockets.awaitClientDealerForOutbound('orders');
  const elapsed = Date.now() - startedAt;

  assert.equal(selected, undefined);
  assert.ok(elapsed >= 5_000, `expected the five second cap to bound the wait, waited ${elapsed}ms`);
  assert.ok(elapsed < 8_000, `expected the cap to end the wait, waited ${elapsed}ms`);
  await sockets.dispose();
});

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
    manager.trySend('orders', 'Notice', { id: 1 }),
    { status: submissionResult.ZLinkSubmitStatus.TargetNotFound }
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

function automaticClientServerSockets() {
  const connections = new Map();
  const history = new Map();
  const ready = new Map();
  const calls = [];
  return {
    calls,
    openClientServerConnection(channelName, connectionId, endpoint, callbacks) {
      const dealer = {
        maxMessageSize: 0x7fff_ffff,
        request(message, callback) {
          const connection = connections.get(connectionId);
          connection.hello = Buffer.from(message.data());
          connection.reply = callback;
          return true;
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
      connection.reply(0, [reply]);
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
    onSendReady() {},
    connect() {},
    disconnect() {},
    send() { return true; },
    request() { return true; },
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
