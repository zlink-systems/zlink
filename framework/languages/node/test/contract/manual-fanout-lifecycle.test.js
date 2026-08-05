const assert = require('node:assert/strict');
const test = require('node:test');
const internal = require('../../packages/framework/dist/internal');
const { ZLinkChannelRuntimeLifecycle } = require(
  '../../packages/framework/dist/runtime/channels/channel-runtime-lifecycle'
);
const { ZLinkSubscriberReceiveLoop } = require(
  '../../packages/framework/dist/runtime/channels/channel-receive-loops'
);
const { endpointConnections } = require(
  '../../packages/framework/dist/contracts/Configuration/RuntimeEndpointConnections'
);

test('manual fanout ignores a stale termination callback after reconnecting the same endpoint', async () => {
  const endpoint = 'tcp://127.0.0.1:9501';
  const attempts = [];
  const closed = [];
  const pollers = [];
  const runtime = createLifecycle({ endpoint, attempts, closed, pollers });
  const subscriber = runtime.registration.channels.get('events').subscriber;
  const handle = endpointConnections(
    subscriber,
    subscriber.manualConnections
  );
  assert.deepEqual(handle.listConnections(), [endpoint]);

  runtime.lifecycle.startSubscriberReceivers(runtime.taskRunner);
  assert.equal(attempts.length, 1);

  attempts[0].callbacks.onTerminated('disconnect');
  handle.disconnect(endpoint);
  handle.connect(endpoint);
  await flush();
  await flush();

  assert.equal(attempts.length, 2);
  assert.deepEqual(closed, ['manual\0events\0tcp://127.0.0.1:9501']);

  attempts[0].callbacks.onTerminated('disconnect');
  await flush();
  assert.equal(attempts.length, 2);
  assert.deepEqual(closed, ['manual\0events\0tcp://127.0.0.1:9501']);

  await runtime.lifecycle.dispose();
  assert.equal(closed.length, 2);
  assert.equal(pollers.length, 2);
  assert.equal(pollers.filter(poller => poller.disposed).length, 2);

  handle.connect(endpoint);
  await flush();
  assert.equal(attempts.length, 2);
});

test('manual fanout closes the socket when receive-loop stop fails and permits a later reconnect', async () => {
  const endpoint = 'tcp://127.0.0.1:9502';
  const attempts = [];
  const closed = [];
  const pollers = [];
  const errors = [];
  const runtime = createLifecycle({
    endpoint,
    attempts,
    closed,
    pollers,
    failFirstPollerDispose: true,
    errors
  });
  const subscriber = runtime.registration.channels.get('events').subscriber;
  const handle = endpointConnections(
    subscriber,
    subscriber.manualConnections
  );
  assert.deepEqual(handle.listConnections(), [endpoint]);

  runtime.lifecycle.startSubscriberReceivers(runtime.taskRunner);
  handle.disconnect(endpoint);
  await flush();
  await flush();

  assert.equal(attempts.length, 1);
  assert.deepEqual(closed, ['manual\0events\0tcp://127.0.0.1:9502']);
  assert.equal(errors.length, 1);

  handle.connect(endpoint);
  await flush();
  assert.equal(attempts.length, 2);

  await runtime.lifecycle.dispose();
  assert.equal(closed.length, 2);
});

test('manual fanout reconnects after a monitor termination even when loop cleanup fails', async () => {
  const endpoint = 'tcp://127.0.0.1:9503';
  const attempts = [];
  const closed = [];
  const pollers = [];
  const errors = [];
  const runtime = createLifecycle({
    endpoint,
    attempts,
    closed,
    pollers,
    failFirstPollerDispose: true,
    errors
  });
  const subscriber = runtime.registration.channels.get('events').subscriber;
  endpointConnections(subscriber, subscriber.manualConnections);

  runtime.lifecycle.startSubscriberReceivers(runtime.taskRunner);
  attempts[0].callbacks.onTerminated('disconnect');
  await flush();
  await flush();

  assert.equal(attempts.length, 2);
  assert.deepEqual(closed, ['manual\0events\0tcp://127.0.0.1:9503']);
  assert.equal(errors.some(({ name }) => name.endsWith(':cleanup')), true);

  await runtime.lifecycle.dispose();
  assert.equal(closed.length, 2);
  assert.equal(pollers.filter(poller => poller.disposed).length, 2);
});

test('manual fanout reconnects one endpoint without closing another endpoint', async () => {
  const firstEndpoint = 'tcp://127.0.0.1:9504';
  const secondEndpoint = 'tcp://127.0.0.1:9505';
  const attempts = [];
  const closed = [];
  const pollers = [];
  const runtime = createLifecycle({
    endpoints: [firstEndpoint, secondEndpoint],
    attempts,
    closed,
    pollers
  });
  const subscriber = runtime.registration.channels.get('events').subscriber;
  const handle = endpointConnections(subscriber, subscriber.manualConnections);
  assert.deepEqual(handle.listConnections(), [firstEndpoint, secondEndpoint]);

  runtime.lifecycle.startSubscriberReceivers(runtime.taskRunner);
  assert.equal(attempts.length, 2);
  attempts.find(({ endpoint }) => endpoint === firstEndpoint).callbacks.onTerminated('disconnect');
  await flush();
  await flush();

  assert.equal(attempts.length, 3);
  assert.deepEqual(closed, ['manual\0events\0tcp://127.0.0.1:9504']);

  await runtime.lifecycle.dispose();
  assert.equal(closed.length, 3);
  assert.equal(closed.includes('manual\0events\0tcp://127.0.0.1:9505'), true);
});

test('manual fanout detaches a retained handle even when no endpoint was active', async () => {
  const endpoint = 'tcp://127.0.0.1:9506';
  const attempts = [];
  const closed = [];
  const pollers = [];
  const runtime = createLifecycle({
    endpoints: [],
    attempts,
    closed,
    pollers
  });
  const subscriber = runtime.registration.channels.get('events').subscriber;
  const handle = endpointConnections(subscriber, subscriber.manualConnections);

  runtime.lifecycle.startSubscriberReceivers(runtime.taskRunner);
  assert.equal(attempts.length, 0);
  await runtime.lifecycle.dispose();

  handle.connect(endpoint);
  await flush();
  assert.equal(attempts.length, 0);
  assert.deepEqual(closed, []);
});

test('manual fanout dispose waits for an in-flight endpoint transition', async () => {
  const endpoint = 'tcp://127.0.0.1:9507';
  const attempts = [];
  const closed = [];
  const pollers = [];
  const runtime = createLifecycle({ endpoint, attempts, closed, pollers });
  const subscriber = runtime.registration.channels.get('events').subscriber;
  const handle = endpointConnections(subscriber, subscriber.manualConnections);
  const originalStop = ZLinkSubscriberReceiveLoop.prototype.stop;
  let stopCalls = 0;
  let releaseStop;
  let stopStarted;
  const stopStartedPromise = new Promise((resolve) => { stopStarted = resolve; });
  const stopGate = new Promise((resolve) => { releaseStop = resolve; });
  ZLinkSubscriberReceiveLoop.prototype.stop = async function controlledStop() {
    stopCalls += 1;
    stopStarted();
    await stopGate;
    return originalStop.call(this);
  };

  try {
    runtime.lifecycle.startSubscriberReceivers(runtime.taskRunner);
    handle.disconnect(endpoint);
    await stopStartedPromise;

    let disposed = false;
    const disposePromise = runtime.lifecycle.dispose().then(() => { disposed = true; });
    await flush();
    assert.equal(disposed, false);

    releaseStop();
    await disposePromise;
    assert.equal(disposed, true);
    assert.equal(stopCalls, 1);
    assert.deepEqual(closed, ['manual\0events\0tcp://127.0.0.1:9507']);
    assert.equal(pollers.filter(poller => poller.disposed).length, 1);
  } finally {
    ZLinkSubscriberReceiveLoop.prototype.stop = originalStop;
  }
});

function createLifecycle({
  endpoint,
  endpoints,
  attempts,
  closed,
  pollers,
  failFirstPollerDispose = false,
  errors = []
}) {
  let pollerDisposeCalls = 0;
  const initialEndpoints = endpoints ?? [endpoint, endpoint];
  const registration = internal.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: initialEndpoints },
        publishHandlers: [{ packetName: 'event', handler: { handle() {} } }]
      }
    },
    ...(initialEndpoints.length === 0 ? { locations: { useInMemoryStores: true } } : {})
  });
  const taskRunner = {
    errorSink: {
      reportRuntimeTaskException(name, error) {
        errors.push({ name, error });
      }
    },
    run() {
      return Promise.resolve();
    }
  };
  const adapter = {
    createReadablePoller() {
      const poller = {
        disposed: false,
        wait() { return false; },
        dispose() {
          this.disposed = true;
          pollerDisposeCalls += 1;
          if (failFirstPollerDispose && pollerDisposeCalls === 1) {
            throw new Error('poller dispose failure');
          }
        }
      };
      pollers.push(poller);
      return poller;
    }
  };
  const sockets = {
    openFanoutSubscriberConnection(channelName, connectionId, remoteEndpoint, callbacks) {
      const subscriber = {
        setChannelName() {},
        setSubscription() {},
        connect() {},
        disconnect() {},
        subscribe() { return false; },
        async dispose() {}
      };
      attempts.push({ channelName, connectionId, endpoint: remoteEndpoint, callbacks, subscriber });
      return subscriber;
    },
    async closeFanoutSubscriberConnection(connectionId) {
      closed.push(connectionId);
    },
    handleFanoutInboundResult() {
      return { consumed: false };
    },
    async dispose() {}
  };
  const dispatchServices = {
    dispatchErrorReporter() { return {}; },
    handlerFilters() { return []; },
    metrics() { return {}; }
  };
  const lifecycle = new ZLinkChannelRuntimeLifecycle({
    registration,
    adapter,
    sockets,
    codecs: { serializers: new Map() },
    dispatchServices,
    spotRoutes: {},
    spotRouteBridges: new Map(),
    spotRouteBridgeRawReplies: { rejectAll() {} }
  });
  lifecycle.taskRunner = taskRunner;
  return { lifecycle, registration, taskRunner };
}

function flush() {
  return new Promise(resolve => setImmediate(resolve));
}
