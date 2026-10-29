const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const { createPublisherRuntime } = require('./fixtures/publisher-runtime');
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const {
  ZLinkNodeRawBindingPort
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const {
  ZLinkNodeEventLoopPoller
} = require('../../packages/framework/dist/runtime/backend/node/node-event-loop-poller');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ZLinkChannelSocketRegistry
} = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
const { ZLinkStreamRuntimeManager } = require('../../packages/framework/dist/runtime/streams');
const {
  wrapSocket
} = require('../../packages/framework/dist/runtime/backend/node/node-socket-backend-adapter');
const {
  ZLinkChannelRuntimeLifecycle
} = require('../../packages/framework/dist/runtime/channels/channel-runtime-lifecycle');
const framework = require('../../packages/framework/dist/internal');
const {
  stopRuntimeParts,
  rollbackRuntimeStart
} = require('../../packages/framework/dist/runtime/host/runtime-shutdown');

test('channel lifecycle retains a failed receive poller owner', async () => {
  let attempts = 0;
  const failure = new Error('first receive poller close failed');
  const registration = framework.createFrameworkRegistration({});
  registration.channels.set('work', {
    server: { bind: 'inproc://close-failure' },
    sendHandlers: [{ packetName: 'Probe', handler() {} }]
  });
  const lifecycle = new ZLinkChannelRuntimeLifecycle({
    registration,
    spotRouteBridges: new Map(),
    spotRoutes: { spotRouteNode() {} },
    dispatchServices: { dispatchErrorReporter() {}, handlerFilters: () => [] },
    sockets: {
      startLocalClientServerConnections() {}, startManualClientServerConnections() {},
      channelRouter: () => ({}), clientServerServerPeersForChannel: () => [],
      async dispose() {}
    },
    adapter: { createReadablePoller: () => ({
      dispose() { if (++attempts === 1) throw failure; }
    }) }
  });
  lifecycle.start({ run: () => Promise.resolve(), errorSink: {} });
  await assert.rejects(lifecycle.dispose(), (error) => error === failure);
  await lifecycle.dispose();
  assert.equal(attempts, 2);
});

test('channel registry owns a socket before startup configuration can fail', async () => {
  const registration = framework.createFrameworkRegistration({});
  registration.channels.set('work', { server: { bind: 'tcp://127.0.0.1:1' } });
  const startupFailure = new Error('socket configuration failed');
  const closeFailure = new Error('first startup socket close failed');
  let attempts = 0;
  const registry = new ZLinkChannelSocketRegistry(
    registration,
    {
      createRouterSocket() {
        return {
          setChannelName() {
            throw startupFailure;
          },
          async dispose() {
            if (++attempts === 1) throw closeFailure;
          }
        };
      }
    },
    {}
  );
  assert.throws(
    () => registry.channelRouter('work'),
    (error) => error === startupFailure
  );
  await assert.rejects(registry.dispose(), (error) => error === closeFailure);
  await registry.dispose();
  assert.equal(attempts, 2);
});

test('fanout registry retains a failed subscriber for subsequent close', async () => {
  let attempts = 0;
  const failure = new Error('first subscriber close failed');
  const monitor = { onEvent() {}, async dispose() {} };
  const subscriber = {
    setChannelName() {}, setSubscription() {}, connect() {}, disconnect() {},
    async dispose() { if (++attempts === 1) throw failure; }
  };
  const registration = framework.createFrameworkRegistration({});
  registration.channels.set('work', { subscriber: {} });
  const registry = new ZLinkChannelSocketRegistry(
    registration, { createSubscriberSocket: () => subscriber }, {},
    { openSocketMonitor: () => monitor }
  );
  registry.openFanoutSubscriberConnection('work', 'publisher', 'inproc://publisher', {});
  await assert.rejects(
    registry.closeFanoutSubscriberConnection('publisher'),
    (error) => error === failure
  );
  await registry.closeFanoutSubscriberConnection('publisher');
  assert.equal(attempts, 2);
  await registry.dispose();
});

test('shutdown reports a failed socket and defers context disposal', async () => {
  const failure = new Error('first runtime close failed');
  let attempts = 0;
  let contextCloses = 0;
  const reported = [];
  const parts = {
    state: {
      listenerTasks: [],
      errorSink: {
        reportRuntimeTaskException(_name, error) {
          reported.push(error);
        }
      },
      async dispose() {
        ++contextCloses;
      }
    },
    locationSnapshot: {},
    channelRuntime: {
      async dispose() {
        if (++attempts === 1) throw failure;
      }
    }
  };
  await assert.rejects(stopRuntimeParts(parts), (error) => error === failure);
  assert.deepEqual(reported, [failure]);
  assert.equal(contextCloses, 0);
  await stopRuntimeParts(parts);
  assert.equal(attempts, 2);
  assert.equal(contextCloses, 1);
});

test('start rollback propagates failed socket cleanup and defers context disposal', async () => {
  const failure = new Error('rollback socket close failed');
  let contextCloses = 0;
  await assert.rejects(
    rollbackRuntimeStart({
      context: {
        async dispose() {
          ++contextCloses;
        }
      },
      channelRuntime: {
        async dispose() {
          throw failure;
        }
      }
    }),
    (error) => error === failure
  );
  assert.equal(contextCloses, 0);
});

test('raw mesh backend retains its failed transport owner', async () => {
  let attempts = 0;
  const failure = new Error('first transport close failed');
  const raw = {
    close() {
      if (++attempts === 1) throw failure;
    }
  };
  const backend = new ZLinkNodeRawMeshBackend('play', 'close-failure', {}, undefined, undefined, undefined, undefined, { raw });
  await assert.rejects(
    () => backend.close(),
    (error) => error === failure
  );
  await backend.close();
  assert.equal(attempts, 2);
});

test('channel registry retains a failed socket for subsequent disposal', async () => {
  let attempts = 0;
  const failure = new Error('first channel socket close failed');
  const registration = framework.createFrameworkRegistration({});
  registration.channels.set('play', { client: {} });
  const registry = new ZLinkChannelSocketRegistry(registration, {
    createDealerSocket: () => ({
      setChannelName() {},
      async dispose() { if (++attempts === 1) throw failure; }
    })
  }, {});
  registry.clientDealer('play');
  await assert.rejects(registry.dispose(), (error) => error === failure);
  await registry.dispose();
  assert.equal(attempts, 2);
});

test('channel registry releases each successfully closed native socket', async (t) => {
  const context = zlink.createContext();
  const first = zlink.createRouterSocket(context);
  const second = zlink.createRouterSocket(context);
  const closeFirst = first.close.bind(first);
  const closeSecond = second.close.bind(second);
  let attempts = 0;
  t.mock.method(second, 'close', () => {
    if (++attempts === 1) throw new Error('second socket close failed');
    closeSecond();
  });
  const registration = framework.createFrameworkRegistration({});
  registration.channels.set('first', { client: {} });
  registration.channels.set('second', { client: {} });
  const sockets = [wrapSocket(first), wrapSocket(second)];
  const registry = new ZLinkChannelSocketRegistry(
    registration, { createDealerSocket: () => sockets.shift() }, {}
  );
  registry.clientDealer('first');
  registry.clientDealer('second');
  t.after(() => {
    closeFirst();
    closeSecond();
    context.close();
  });
  await assert.rejects(registry.dispose());
  await registry.dispose();
  assert.equal(attempts, 2);
});

test('stream manager retains a failed socket for subsequent disposal', async () => {
  let attempts = 0;
  const failure = new Error('first stream socket close failed');
  const startupFailure = new Error('stop after the stream socket is owned');
  const registration = framework.createFrameworkRegistration({});
  registration.streamNodes.set('play', { bind: 'tcp://127.0.0.1:0' });
  const manager = new ZLinkStreamRuntimeManager({
    registration,
    backendAdapterFactory: {
      createMonitoringAdapter: () => ({}),
      createStreamAdapter: () => ({ createStreamSocket: () => ({
        bind() { throw startupFailure; },
        async dispose() { if (++attempts === 1) throw failure; }
      }) })
    }
  });
  assert.throws(() => manager.start(), (error) => error === startupFailure);
  await assert.rejects(manager.dispose(), (error) => error === failure);
  await manager.dispose();
  assert.equal(attempts, 2);
});

for (const kind of ['Router', 'Dealer']) {
  test(`raw ${kind} socket remains owned after close failure`, (t) => {
    const context = zlink.createContext();
    const create = zlink[`create${kind}Socket`];
    let socket;
    let attempts = 0;
    const failure = new Error('first socket close failed');
    t.mock.method(zlink, `create${kind}Socket`, (value) => {
      socket = create(value);
      const close = socket.close.bind(socket);
      t.mock.method(socket, 'close', () => {
        if (++attempts === 1) throw failure;
        close();
      });
      return socket;
    });
    const host = new ZLinkNodeRawBindingPort(context).createHost();
    const port = host[`create${kind}`]();
    t.after(() => {
      socket.close();
      context.close();
    });
    assert.throws(
      () => port.close(),
      (error) => error.errors.includes(failure)
    );
    host.close();
    assert.equal(attempts, 2);
  });
}

test('raw host retains a failed resource and defers owned context close', (t) => {
  const context = zlink.createContext();
  const contextClose = context.close.bind(context);
  let contextAttempts = 0;
  t.mock.method(context, 'close', () => {
    ++contextAttempts;
  });
  t.mock.method(zlink, 'createContext', () => context);
  const host = new ZLinkNodeRawBindingPort().createHost();
  const port = host.createRouter();
  const close = port.close.bind(port);
  let attempts = 0;
  t.mock.method(port, 'close', () => {
    if (++attempts === 1) throw new Error('first port close failed');
    close();
  });
  t.after(() => {
    close();
    contextClose();
  });
  assert.throws(() => host.close(), AggregateError);
  assert.equal(contextAttempts, 0);
  host.close();
  assert.equal(attempts, 2);
  assert.equal(contextAttempts, 1);
});

test('raw monitor remains owned after close failure', (t) => {
  const context = zlink.createContext();
  const create = zlink.createRouterSocket;
  let socket;
  let monitor;
  let attempts = 0;
  t.mock.method(zlink, 'createRouterSocket', (value) => {
    socket = create(value);
    const open = socket.monitorOpen.bind(socket);
    t.mock.method(socket, 'monitorOpen', () => {
      monitor = open();
      const close = monitor.close.bind(monitor);
      t.mock.method(monitor, 'close', () => {
        if (++attempts === 1) throw new Error('first monitor close failed');
        close();
      });
      return monitor;
    });
    return socket;
  });
  const host = new ZLinkNodeRawBindingPort(context).createHost();
  const port = host.createRouter();
  const rawMonitor = port.monitor();
  t.after(() => {
    monitor.close();
    socket.close();
    context.close();
  });
  assert.throws(() => rawMonitor.close(), /first monitor/);
  host.close();
  assert.equal(attempts, 2);
});

test('event loop poller retries a failed close on subsequent disposal', (t) => {
  const context = zlink.createContext();
  const socket = zlink.createRouterSocket(context);
  const create = zlink.createPoller;
  let nativePoller;
  let attempts = 0;
  t.mock.method(zlink, 'createPoller', () => {
    nativePoller = create();
    const close = nativePoller.close.bind(nativePoller);
    t.mock.method(nativePoller, 'close', () => {
      if (++attempts === 1) throw new Error('first poller close failed');
      close();
    });
    return nativePoller;
  });
  const poller = new ZLinkNodeEventLoopPoller(socket, true, () => {});
  t.after(() => {
    nativePoller.close();
    socket.close();
    context.close();
  });
  assert.throws(() => poller.dispose(), /first poller/);
  poller.dispose();
  assert.equal(attempts, 2);
});

const { ZLinkSpotNodeRuntimeManager } = require('../../packages/framework/dist/runtime/spots/spot-node-runtime-manager');

test('channel disposal rejects new manual work and shares its in-flight close', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  let attempts = 0;
  const lifecycle = new ZLinkChannelRuntimeLifecycle({ spotRouteBridges: new Map(), sockets: { async dispose() { ++attempts; await gate; } } });
  const first = lifecycle.dispose();
  const second = lifecycle.dispose();
  try {
    lifecycle.requestManualFanoutConnect('work', 'tcp://127.0.0.1:1', {}, {});
    assert.equal(lifecycle.manualFanoutSubscribers.size, 0);
  } finally { release(); }
  await Promise.all([first, second]);
  assert.equal(attempts, 1);
});

test('spot disposal rejects work while close is pending and shares close', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  let attempts = 0;
  const managerOptions = { detachedTaskRunner: {} };
  const manager = await createPublisherRuntime('mesh', { async close() { ++attempts; await gate; } }, managerOptions);
  const first = manager.dispose();
  const second = manager.dispose();
  try {
    await assert.rejects(manager.publish('mesh', 'work', 'topic', undefined, {}));
  } finally { release(); }
  await Promise.all([first, second]);
  assert.equal(attempts, 1);
});

for (const kind of ['client', 'fanout']) {
  for (const stage of (kind === 'client' ? ['poller', 'monitor'] : ['monitor'])) {
    test(`${kind} creation failure at ${stage} removes live connection and retains failed cleanup`, async () => {
      const registration = framework.createFrameworkRegistration({});
      registration.channels.set('work', { client: {}, subscriber: {}, subscriptions: [] });
      const failure = new Error('creation failed');
      const closeFailure = new Error('first cleanup failed');
      let attempts = 0;
      const reported = [];
      const socket = { setChannelName() {}, setRoutingId() {}, setSubscription() {}, async dispose() { if (++attempts === 1) throw closeFailure; } };
      const poller = { dispose() {} };
      const registry = new ZLinkChannelSocketRegistry(registration, {
        createDealerSocket() { return socket; }, createSubscriberSocket() { return socket; },
        createReadablePoller() { if (stage === 'poller') throw failure; return poller; }
      }, {}, { openSocketMonitor() { throw failure; } }, (error) => reported.push(error));
      assert.throws(() => kind === 'client'
        ? registry.openClientServerConnection('work', 'broken', 'tcp://127.0.0.1:1', {})
        : registry.openFanoutSubscriberConnection('work', 'broken', 'tcp://127.0.0.1:1', {}), (error) => error === failure);
      assert.equal(registry[inspectRuntime]().clientServerConnections, 0);
      assert.equal(registry[inspectRuntime]().fanoutConnections, 0);
      await new Promise((resolve) => setImmediate(resolve));
      assert.equal(attempts, 1);
      assert.ok(reported.includes(closeFailure));
      await registry.dispose();
      assert.equal(attempts, 2);
    });
  }
}

const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  M6A_SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

test('raw runtime retains the same host after explicit close failure without reopening admission', async (t) => {
  const context = zlink.createContext();
  const port = new ZLinkNodeRawBindingPort(context);
  const createHost = port.createHost.bind(port);
  const createRouterSocket = zlink.createRouterSocket;
  const failure = new Error('first raw runtime socket close failed');
  let host;
  let router;
  let socket;
  let closeSocket;
  let raw;
  let attempts = 0;
  let nativeCloses = 0;
  let reentrantReturns = 0;
  t.mock.method(port, 'createHost', () => {
    host = createHost();
    const createRouter = host.createRouter.bind(host);
    t.mock.method(host, 'createRouter', () => {
      router = createRouter();
      return router;
    });
    return host;
  });
  t.mock.method(zlink, 'createRouterSocket', (value) => {
    socket = createRouterSocket(value);
    closeSocket = socket.close.bind(socket);
    t.mock.method(socket, 'close', function () {
      assert.equal(this, socket);
      if (++attempts === 1) {
        raw.close();
        ++reentrantReturns;
        assert.equal(attempts, 1);
        throw failure;
      }
      closeSocket();
      ++nativeCloses;
    });
    return socket;
  });
  const failures = [];
  try {
    raw = new RawServiceMeshRuntime({
      descriptor: {
        meshName: 'raw-close-failure',
        nodeRoutingId: 'source',
        lifecycleGeneration: 1n,
        descriptorRevision: 1n,
        advertisedEndpoint: `inproc://raw-close-failure-${process.pid}`,
        channels: [],
        state: 'serving',
        securityIdentity: 'default',
        applicationVersion: 1n,
        protocolCapabilities: [M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
        objectRole: 'server',
        placementWeight: 1,
        activeCapacityLimit: 100,
        pendingCapacityLimit: 16,
        activeCapacityUsed: 0,
        pendingCapacityUsed: 0
      },
      applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
      bindingPort: port
    });
    raw.start();
    const receive = t.mock.method(router, 'receive');
    const send = t.mock.method(router, 'submitSend');
    const routes = t.mock.method(router, 'routesSnapshot');
    const containsFailure = (error) => error === failure ||
      (error instanceof AggregateError && error.errors.some(containsFailure));
    assert.throws(() => raw.close(), (error) =>
      error instanceof AggregateError && containsFailure(error));
    assert.equal(attempts, 1);
    assert.equal(reentrantReturns, 1);
    assert.equal(nativeCloses, 0);
    assert.equal(raw.announcePeer('peer'), false);
    assert.equal(await raw.pumpBatch(true, true), false);
    assert.equal(receive.mock.callCount(), 0);
    assert.equal(send.mock.callCount(), 0);
    assert.equal(routes.mock.callCount(), 0);
    raw.close();
    assert.equal(attempts, 2);
    assert.equal(nativeCloses, 1);
    raw.close();
    assert.equal(attempts, 2);
    assert.equal(nativeCloses, 1);
  } catch (error) {
    failures.push(error);
  } finally {
    for (const close of [() => raw?.close(), () => host?.close(),
      () => {
        if (closeSocket !== undefined && nativeCloses === 0) {
          closeSocket();
          ++nativeCloses;
        }
      }, () => context.close()]) {
      try {
        close();
      } catch (error) {
        failures.push(error);
      }
    }
  }
  if (failures.length === 1) throw failures[0];
  if (failures.length > 1) {
    throw new AggregateError(failures, 'raw runtime test or resource cleanup failed');
  }
});
