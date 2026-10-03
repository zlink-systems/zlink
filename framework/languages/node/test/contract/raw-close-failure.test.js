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
  const lifecycle = new ZLinkChannelRuntimeLifecycle({
    spotRouteBridges: new Map(),
    sockets: { async dispose() {} }
  });
  let attempts = 0;
  const failure = new Error('first receive poller close failed');
  lifecycle.channelReceiveLoops.push({
    async stop() {
      if (++attempts === 1) throw failure;
    }
  });
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
  const registry = new ZLinkChannelSocketRegistry(
    framework.createFrameworkRegistration({}),
    {},
    {}
  );
  let attempts = 0;
  const failure = new Error('first subscriber close failed');
  const monitor = { async dispose() {} };
  registry.fanoutConnections.set('publisher', {
    monitor,
    subscriber: {
      disconnect() {},
      async dispose() {
        if (++attempts === 1) throw failure;
      }
    }
  });
  registry.ownedMonitors.add(monitor);
  await assert.rejects(
    registry.closeFanoutSubscriberConnection('publisher'),
    (error) => error === failure
  );
  await registry.closeFanoutSubscriberConnection('publisher');
  assert.equal(attempts, 2);
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

test('raw mesh backend retains its failed transport owner', () => {
  const backend = new ZLinkNodeRawMeshBackend('play', 'close-failure', {});
  let attempts = 0;
  const failure = new Error('first transport close failed');
  backend.runtime = {
    close() {
      if (++attempts === 1) throw failure;
    }
  };
  assert.throws(
    () => backend.close(),
    (error) => error === failure
  );
  backend.close();
  assert.equal(attempts, 2);
});

test('channel registry retains a failed socket for subsequent disposal', async () => {
  const registry = new ZLinkChannelSocketRegistry(
    framework.createFrameworkRegistration({}),
    {},
    {}
  );
  let attempts = 0;
  const failure = new Error('first channel socket close failed');
  registry.clientDealers.set('play', {
    async dispose() {
      if (++attempts === 1) throw failure;
    }
  });
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
  const registry = new ZLinkChannelSocketRegistry(
    framework.createFrameworkRegistration({}),
    {},
    {}
  );
  registry.clientDealers.set('first', wrapSocket(first));
  registry.clientDealers.set('second', wrapSocket(second));
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
  const manager = new ZLinkStreamRuntimeManager({});
  let attempts = 0;
  const failure = new Error('first stream socket close failed');
  manager.nodes.set('play', {
    nativeSessionServices: [],
    socket: {
      async dispose() {
        if (++attempts === 1) throw failure;
      }
    }
  });
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
