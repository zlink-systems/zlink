const assert = require('node:assert/strict');
const { once } = require('node:events');
const net = require('node:net');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');
const framework = require('../../packages/framework/dist/internal');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const streamReassembler = require(
  '../../packages/framework/dist/runtime/streams/stream-frame-reassembler'
);
const streamDispatchCapacity = require(
  '../../packages/framework/dist/runtime/streams/stream-dispatch-capacity'
);
const backend = require('../../packages/framework/dist/runtime/backend');
const nodeMonitorBackend = require('../../packages/framework/dist/runtime/backend/node/node-monitor-backend-adapter');

test('node monitor adapter preserves the opaque native session routing id', () => {
  let nativeHandler;
  let observed;
  const routingId = zlink.RoutingId.from(2);
  const monitor = nodeMonitorBackend.wrapMonitorSocket({
    close() {},
    recv() { return null; },
    onEvent(handler) { nativeHandler = handler; }
  });
  monitor.onEvent((event) => { observed = event; });

  nativeHandler({
    event: zlink.MonitorEventType.Disconnected,
    value: 0,
    routingId,
    localAddr: 'tcp://127.0.0.1:9000',
    remoteAddr: 'tcp://127.0.0.1:50000'
  });

  assert.equal(observed.routingId, routingId);
});

test('STREAM runtime registers its monitor handler once across repeated starts', async () => {
  let monitorRegistrations = 0;
  const runtime = createStreamRuntime({
    socket: new FakeStreamSocket(),
    monitor: {
      onEvent() {
        monitorRegistrations += 1;
      }
    }
  });

  runtime.start();
  runtime.start();
  assert.equal(monitorRegistrations, 1);
  await runtime.dispose();
  runtime.start();
  assert.equal(monitorRegistrations, 1);
});

test('ConnectionReady before the first packet keeps the native routing id for replies', async () => {
  const socket = new FakeStreamSocket();
  const routingId = zlink.RoutingId.from(2);
  let monitorHandler;
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: {
      createTextMessage(payload) { return zlink.Message.from(Buffer.from(payload)); },
      createBinaryMessage(payload) { return zlink.Message.from(Buffer.from(payload)); }
    }
  });
  const replied = new Promise((resolve) => {
    const runtime = createStreamRuntime({
      socket,
      bindingRuntime,
      monitor: { onEvent(handler) { monitorHandler = handler; } },
      headerDecoder: (header) => protocolCodecs.ZlinkStreamHeaderCodec.decode(header.data()),
      sessionFactory(context) {
        return {
          context,
          async onDispatch() {
            await context.client.reply('ready-first').submit();
            resolve(runtime);
          }
        };
      }
    });
    runtime.start();
    monitorHandler({
      nativeEvent: framework.ZLinkSocketNativeEventType.ConnectionReady,
      routingId,
      localAddr: 'tcp://local',
      remoteAddr: 'tcp://remote',
      value: 0
    });
    socket.emitPacket(routingId, fakeHeader({
      kind: connector.ZlinkStreamMessageKind.Request,
      requestSeq: 1n,
      name: 'ReadyFirst'
    }), fakeMessage('payload'));
  });

  const runtime = await replied;
  assert.equal(socket.sent.length, 1);
  assert.equal(socket.sent[0].routingId, routingId);
  await runtime.dispose();
});

test('actor session lifecycle serializes disconnect and replacement bind for the same actor', async () => {
  const lifecycle = new framework.ZLinkActorSessionLifecycleCoordinator();
  const events = [];
  let releaseDisconnect;
  const disconnectCanFinish = new Promise((resolve) => { releaseDisconnect = resolve; });
  let disconnectStarted;
  const disconnectDidStart = new Promise((resolve) => { disconnectStarted = resolve; });

  const disconnect = lifecycle.run('actor-1', async () => {
    events.push('disconnect:start');
    disconnectStarted();
    await disconnectCanFinish;
    events.push('disconnect:end');
  });
  await disconnectDidStart;
  const bind = lifecycle.run('actor-1', async () => {
    events.push('bind');
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(events, ['disconnect:start']);

  releaseDisconnect();
  await Promise.all([disconnect, bind]);
  assert.deepEqual(events, ['disconnect:start', 'disconnect:end', 'bind']);
});

test('session handler registry uses packet metadata and closes registration after session creation', async () => {
  const socket = new FakeStreamSocket();
  const handled = [];
  class RenamedHandler {
    async handle(_context, dispatch, message) {
      handled.push([dispatch.packetName, message.decode()]);
    }
  }
  class DuplicateHandler {
    async handle() {}
  }
  class LateHandler {
    async handle() {}
  }
  framework.ZLinkPacket('session.contract')(RenamedHandler);
  framework.ZLinkPacket('session.contract')(DuplicateHandler);
  framework.ZLinkPacket('session.late')(LateHandler);

  const runtime = new framework.ZLinkStreamSessionRuntime({
    socket,
    sessionFactory(context) {
      context.handlers.addHandler(RenamedHandler);
      return { context };
    }
  }, zlink.RoutingId.from(9));

  await runtime.session;
  assert.equal(await runtime.context.handlers.tryHandle({
    packetName: 'session.contract',
    metadata: new Map(),
    canReply: false
  }, { decode: () => 'payload' }), true);
  assert.deepEqual(handled, [['session.contract', 'payload']]);
  assert.throws(
    () => runtime.context.handlers.addHandler(DuplicateHandler),
    /registration is closed/i
  );
  assert.throws(
    () => runtime.context.handlers.addHandler(LateHandler),
    /registration is closed/i
  );

  await runtime.dispose();

  assert.throws(() => {
    new framework.ZLinkStreamSessionRuntime({
      socket: new FakeStreamSocket(),
      sessionFactory(context) {
        context.handlers.addHandler(RenamedHandler);
        context.handlers.addHandler(DuplicateHandler);
        return { context };
      }
    }, zlink.RoutingId.from(10));
  }, /already registered/i);
});

test('session handler registry resolves handler instances through the runtime provider resolver', async () => {
  const handled = [];
  const dependency = { value: 'resolved' };
  class InjectedHandler {
    constructor(value) {
      this.value = value;
    }
    async handle(_context, _dispatch, message) {
      handled.push([this.value.value, message.decode()]);
    }
  }
  framework.ZLinkPacket('session.injected')(InjectedHandler);
  let createCount = 0;
  const runtime = new framework.ZLinkStreamSessionRuntime({
    socket: new FakeStreamSocket(),
    providerResolver: {
      create(type) {
        createCount += 1;
        assert.equal(type, InjectedHandler);
        return new InjectedHandler(dependency);
      }
    },
    sessionFactory(context) {
      context.handlers.addHandler(InjectedHandler);
      return { context };
    }
  }, zlink.RoutingId.from(11));

  await runtime.session;
  assert.equal(await runtime.context.handlers.tryHandle({
    packetName: 'session.injected',
    metadata: new Map(),
    canReply: false
  }, { decode: () => 'payload' }), true);
  assert.deepEqual(handled, [['resolved', 'payload']]);
  await runtime.context.handlers.tryHandle({
    packetName: 'session.injected',
    metadata: new Map(),
    canReply: false
  }, { decode: () => 'again' });
  assert.equal(createCount, 1);
  assert.deepEqual(handled, [['resolved', 'payload'], ['resolved', 'again']]);
  await runtime.dispose();
});

test('stream session node runtime dispatches framed packets through one session context', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let dispatchCount = 0;
  let dispatchesDone;
  const dispatchesDonePromise = new Promise((resolve) => {
    dispatchesDone = resolve;
  });
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          events.push(['connected', ctx.sessionId, ctx.localAddr, ctx.remoteAddr]);
        },
        async onDispatch(header, payload) {
          events.push(['dispatch', header.packetName, payload.decode()]);
          dispatchCount += 1;
          if (dispatchCount === 2) {
            dispatchesDone();
          }
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  runtime.markConnected('session-a', 'tcp://local', 'tcp://remote');
  socket.emitPacket('session-a', fakeHeader({ name: 'Ready' }), fakeMessage('one'));
  socket.emitPacket('session-a', fakeHeader({ name: 'Move' }), fakeMessage('two'));
  await dispatchesDonePromise;
  await runtime.dispose();

  assert.deepEqual(events, [
    ['connected', 'session-a', 'tcp://local', 'tcp://remote'],
    ['dispatch', 'Ready', 'one'],
    ['dispatch', 'Move', 'two'],
    ['disconnected', 'session-a']
  ]);
});

test('STREAM application claim remains active through async handler terminal cleanup', async () => {
  const socket = new FakeStreamSocket();
  const gate = new framework.ZLinkRuntimeAdmissionGate();
  gate.register('game');
  let entered;
  const didEnter = new Promise((resolve) => { entered = resolve; });
  let release;
  const canFinish = new Promise((resolve) => { release = resolve; });
  const runtime = createStreamRuntime({
    socket,
    claimApplicationWork: () => gate.claim('game', 'STREAM dispatch'),
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          entered();
          await canFinish;
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-a', fakeHeader({ name: 'Work' }), fakeMessage('payload'));
  await didEnter;
  gate.seal('game');
  assert.equal(gate.pending('game'), 1);
  let zero = false;
  const drained = gate.awaitZero('game').then(() => { zero = true; });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(zero, false);
  release();
  await drained;
  assert.equal(gate.pending('game'), 0);
  await runtime.dispose();
});

test('stream session runtime sends heartbeat ping and consumes pong outside application dispatch', async () => {
  const socket = new FakeStreamSocket();
  const clock = new FakeLivenessClock();
  let dispatches = 0;
  const runtime = createStreamRuntime({
    socket,
    livenessClock: clock,
    sessionFactory(context) {
      return {
        context,
        async onDispatch() { dispatches += 1; }
      };
    }
  });

  runtime.start();
  runtime.markConnected('heartbeat-session');
  await clock.flush();
  await clock.advance(1000);
  assert.equal(controlHeader(socket.sent[0]).name, '$zlink.heartbeat.ping');

  socket.emitPacket('heartbeat-session', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Control,
    codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: '$zlink.heartbeat.pong'
  }), fakeMessage(''));
  await clock.flush();
  await waitForReceive(socket);

  socket.emitPacket('heartbeat-session', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Control,
    codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: '$zlink.heartbeat.ping'
  }), fakeMessage(''));
  await clock.flush();
  await waitForReceive(socket);

  assert.equal(dispatches, 0);
  assert.equal(controlHeader(socket.sent.at(-1)).name, '$zlink.heartbeat.pong');
  assert.deepEqual(socket.disconnects, []);
  await runtime.dispose();
});

test('stream control frames do not consume the application HWM budget', async () => {
  const socket = new FakeStreamSocket();
  const budget = new framework.ZLinkInboundDispatchBudget(1n);
  let pauseTransitions = 0;
  budget.onPause(() => { pauseTransitions += 1; });
  const runtime = createStreamRuntime({
    socket,
    inboundDispatchBudget: budget,
    sessionFactory(context) { return { context }; }
  });

  runtime.start();
  const header = fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Control,
    codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: '$zlink.heartbeat.ping'
  });
  const frame = rawStreamFrame(header.data(), Buffer.from('invalid-control-payload'));
  header.close();
  socket.emitRaw('control-budget-peer', [frame]);
  await waitForCondition(
    () => socket.disconnects.length === 1,
    'invalid control frame isolation'
  );

  assert.equal(pauseTransitions, 0);
  assert.equal(budget.pendingPayloadBytes, 0n);
  await runtime.dispose();
});

test('stream session runtime closes an unanswered heartbeat with heartbeat_timeout', async () => {
  const socket = new FakeStreamSocket();
  const clock = new FakeLivenessClock();
  const runtime = createStreamRuntime({
    socket,
    livenessClock: clock,
    sessionFactory(context) { return { context }; }
  });

  runtime.start();
  runtime.markConnected('heartbeat-timeout-session');
  await clock.flush();
  await clock.advance(6000);

  const closing = decodeSessionClosing(socket.sent.at(-1));
  assert.equal(closing.header.name, 'session-closing');
  assert.equal(closing.payload[1], 3);
  assert.deepEqual(socket.disconnects, ['heartbeat-timeout-session']);
  await runtime.dispose();
});

test('stream session runtime closes application-idle sessions with idle_timeout', async () => {
  const socket = new FakeStreamSocket();
  const clock = new FakeLivenessClock();
  const runtime = createStreamRuntime({
    socket,
    livenessClock: clock,
    sessionFactory(context) { return { context }; }
  });

  runtime.start();
  runtime.markConnected('idle-timeout-session');
  await clock.flush();
  for (let second = 0; second < 29; second += 1) {
    await clock.advance(1000);
    socket.emitPacket('idle-timeout-session', fakeHeader({
      kind: connector.ZlinkStreamMessageKind.Control,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: '$zlink.heartbeat.pong'
    }), fakeMessage(''));
    await clock.flush();
    await waitForReceive(socket);
  }
  await clock.advance(1000);

  const closing = decodeSessionClosing(socket.sent.at(-1));
  assert.equal(closing.payload[1], 2);
  assert.deepEqual(socket.disconnects, ['idle-timeout-session']);
  await runtime.dispose();
});

test('stream session node runtime serializes dispatch and disconnect callbacks per session', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let releaseFirst;
  const firstCanFinish = new Promise((resolve) => {
    releaseFirst = resolve;
  });
  let firstStarted;
  const firstStartedPromise = new Promise((resolve) => {
    firstStarted = resolve;
  });
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['dispatch:start', header.packetName, payload.decode()]);
          if (header.packetName === 'First') {
            firstStarted();
            await firstCanFinish;
          }
          events.push(['dispatch:end', header.packetName]);
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-serial', fakeHeader({ name: 'First' }), fakeMessage('one'));
  await firstStartedPromise;
  socket.emitPacket('session-serial', fakeHeader({ name: 'Second' }), fakeMessage('two'));
  await waitForReceive(socket);
  runtime.markDisconnected('session-serial');
  assert.deepEqual(events, [['dispatch:start', 'First', 'one']]);

  releaseFirst();
  await runtime.dispose();

  assert.deepEqual(events, [
    ['dispatch:start', 'First', 'one'],
    ['dispatch:end', 'First'],
    ['disconnected', 'session-serial'],
    ['dispatch:start', 'Second', 'two'],
    ['dispatch:end', 'Second']
  ]);
});

test('stream session node runtime ignores unmatched monitor disconnect when multiple sessions exist', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: {
      onEvent(handler) {
        monitorHandler = handler;
      }
    },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          events.push(['connected', ctx.sessionId]);
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  runtime.markConnected('session-stale-a', 'tcp://local-a', 'tcp://remote-a');
  runtime.markConnected('session-stale-b', 'tcp://local-b', 'tcp://remote-b');
  await new Promise((resolve) => setImmediate(resolve));

  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: undefined,
    remoteAddr: undefined,
    routingId: undefined
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(events, [
    ['connected', 'session-stale-a'],
    ['connected', 'session-stale-b']
  ]);
  await runtime.dispose();
});

test('stream session node runtime uses monitor routing id to disconnect exactly one of multiple sessions', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: { onEvent(handler) { monitorHandler = handler; } },
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) { events.push(['connected', ctx.sessionId]); },
        async onDisconnected(ctx) { events.push(['disconnected', ctx.sessionId]); }
      };
    }
  });

  runtime.start();
  runtime.markConnected('session-a', 'tcp://local', 'tcp://remote-a');
  runtime.markConnected('session-b', 'tcp://local', 'tcp://remote-b');
  await new Promise((resolve) => setImmediate(resolve));
  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://remote-a',
    routingId: 'session-a'
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.deepEqual(events, [
    ['connected', 'session-a'],
    ['connected', 'session-b'],
    ['disconnected', 'session-a']
  ]);
  assert.equal(runtime.findSession('session-b').isDisconnected, false);
  await runtime.dispose();
});

test('stream session node runtime maps a unique endpoint when a disconnect lacks routing id', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: { onEvent(handler) { monitorHandler = handler; } },
    sessionFactory(context) {
      return {
        context,
        async onDisconnected(ctx) { events.push(['disconnected', ctx.sessionId]); }
      };
    }
  });

  runtime.start();
  runtime.markConnected('old-session', 'tcp://local', 'tcp://old');
  runtime.markConnected('fresh-session', 'tcp://local', 'tcp://fresh');
  await new Promise((resolve) => setImmediate(resolve));
  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://fresh',
    routingId: undefined
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.deepEqual(events, [['disconnected', 'fresh-session']]);
  await runtime.dispose();
});

test('stream session node runtime maps endpointless monitor disconnect to a single session', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: {
      onEvent(handler) {
        monitorHandler = handler;
      }
    },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          events.push(['connected', ctx.sessionId]);
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  runtime.markConnected('session-live', 'tcp://local-live', 'tcp://remote-live');
  await new Promise((resolve) => setImmediate(resolve));

  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: undefined,
    remoteAddr: undefined,
    routingId: undefined
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(events, [
    ['connected', 'session-live'],
    ['disconnected', 'session-live']
  ]);
  await runtime.dispose();
});

test('stream session node runtime consumes a removed session tombstone before a late endpointless disconnect', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: { onEvent(handler) { monitorHandler = handler; } },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header) { events.push(['dispatch', context.sessionId, header.packetName]); },
        async onDisconnected(ctx) { events.push(['disconnected', ctx.sessionId]); }
      };
    }
  });

  runtime.start();
  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.ConnectionReady,
    value: 0,
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://old',
    routingId: undefined
  });
  socket.emitPacket('session-old', fakeHeader({ name: 'Old' }), fakeMessage('old'));
  await waitForReceive(socket);
  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://old',
    routingId: undefined
  });
  await new Promise((resolve) => setImmediate(resolve));

  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.ConnectionReady,
    value: 0,
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://fresh',
    routingId: undefined
  });
  socket.emitPacket('session-fresh', fakeHeader({ name: 'Fresh' }), fakeMessage('fresh'));
  await waitForReceive(socket);
  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: undefined,
    remoteAddr: undefined,
    routingId: undefined
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.deepEqual(events, [
    ['dispatch', 'session-old', 'Old'],
    ['disconnected', 'session-old'],
    ['dispatch', 'session-fresh', 'Fresh']
  ]);
  assert.equal(runtime.findSession('session-fresh').isDisconnected, false);
  await runtime.dispose();
});

test('stream session node runtime cancels endpointless disconnect when connection-ready follows immediately', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: {
      onEvent(handler) {
        monitorHandler = handler;
      }
    },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          events.push(['connected', ctx.sessionId]);
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  runtime.markConnected('session-live');
  await new Promise((resolve) => setImmediate(resolve));

  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: undefined,
    remoteAddr: undefined,
    routingId: undefined
  });
  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.ConnectionReady,
    value: 0,
    localAddr: undefined,
    remoteAddr: undefined,
    routingId: undefined
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(events, [
    ['connected', 'session-live']
  ]);
  await runtime.dispose();
});

test('stream session node runtime cancels endpointless disconnect when another packet arrives before it is applied', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: {
      onEvent(handler) {
        monitorHandler = handler;
      }
    },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          events.push(['connected', ctx.sessionId]);
        },
        async onDispatch(header, payload) {
          events.push(['dispatch', context.sessionId, header.packetName, payload.decode()]);
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  runtime.markConnected('session-live');
  await new Promise((resolve) => setImmediate(resolve));

  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: undefined,
    remoteAddr: undefined,
    routingId: undefined
  });
  socket.emitPacket('session-next', fakeHeader({ name: 'Ready' }), fakeMessage('next'));
  await waitForReceive(socket);
  assert.deepEqual(events, [
    ['connected', 'session-live'],
    ['dispatch', 'session-next', 'Ready', 'next']
  ]);
  await runtime.dispose();
});

test('stream session node runtime ignores monitor disconnect whose endpoint does not match the single session', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let monitorHandler;
  const runtime = createStreamRuntime({
    socket,
    monitor: {
      onEvent(handler) {
        monitorHandler = handler;
      }
    },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          events.push(['connected', ctx.sessionId]);
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  runtime.markConnected('session-live', 'tcp://local-live', 'tcp://remote-live');
  await new Promise((resolve) => setImmediate(resolve));

  monitorHandler({
    nativeEvent: framework.ZLinkSocketNativeEventType.Disconnected,
    value: 0,
    localAddr: 'tcp://local-stale',
    remoteAddr: 'tcp://remote-stale',
    routingId: undefined
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(events, [
    ['connected', 'session-live']
  ]);
  await runtime.dispose();
});

test('stream session node runtime does not invoke user callbacks inside transport callback', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['dispatch', header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-deferred', fakeHeader({ name: 'Deferred' }), fakeMessage('body'));
  assert.deepEqual(events, []);
  await waitForReceive(socket);
  await waitForCondition(() => events.length === 1, 'deferred stream dispatch');

  await runtime.dispose();
  assert.deepEqual(events, [['dispatch', 'Deferred', 'body']]);
});

test('stream session node runtime reassembles a frame split across recv calls', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push([header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  const frame = rawStreamFrame(
    fakeHeader({ name: 'Segmented' }).data(),
    Buffer.from('payload')
  );
  socket.emitRaw('segmented-peer', [frame.subarray(0, 4)]);
  await waitForReceive(socket);
  assert.deepEqual(events, []);

  socket.emitRaw('segmented-peer', [frame.subarray(4)]);
  await waitForReceive(socket);
  await waitForCondition(() => events.length === 1, 'segmented stream dispatch');
  assert.deepEqual(events, [['Segmented', 'payload']]);
  await runtime.dispose();
});

test('segmented STREAM frames reuse the transferred assembled backing buffer', () => {
  const reassembler = new streamReassembler.ZLinkStreamFrameReassembler();
  const first = rawStreamFrame(Buffer.from('first-header'), Buffer.from('one'));
  const second = rawStreamFrame(Buffer.from('second-header'), Buffer.from('two'));
  const combined = Buffer.concat([first, second]);

  reassembler.append(combined.subarray(0, 4));
  reassembler.append(combined.subarray(4));
  const firstResult = reassembler.next();
  const secondResult = reassembler.next();

  assert.equal(firstResult.kind, 'frame');
  assert.equal(secondResult.kind, 'frame');
  assert.equal(firstResult.frame.payload.toString(), 'one');
  assert.equal(secondResult.frame.payload.toString(), 'two');
  assert.equal(firstResult.frame.payload.buffer, secondResult.frame.payload.buffer);
});

test('stream session node runtime rejects MaxMessageSize violations on direct and copied paths', async () => {
  for (const segmented of [false, true]) {
    const socket = new FakeStreamSocket();
    socket.maxMessageSize = 512;
    const events = [];
    const runtime = createStreamRuntime({
      socket,
      headerDecoder: (header) => ({ name: header.getString() }),
      sessionFactory(context) {
        return {
          context,
          async onDispatch(header, payload) {
            events.push([header.packetName, payload.size()]);
          }
        };
      }
    });

    runtime.start();
    const frame = rawStreamFrame(
      fakeHeader({ name: 'Bounded' }).data(),
      Buffer.alloc(512)
    );
    if (segmented) {
      socket.emitRaw('oversized-peer', [frame.subarray(0, 4)]);
      await waitForReceive(socket);
      socket.emitRaw('oversized-peer', [frame.subarray(4)]);
    } else {
      socket.emitRaw('oversized-peer', [frame]);
    }
    await waitForCondition(() => socket.disconnects.length === 1, 'MaxMessageSize rejection');
    assert.deepEqual(events, []);
    await runtime.dispose();
  }
});

test('stream session node runtime dispatches every frame contained in one raw part', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push([header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  const first = rawStreamFrame(fakeHeader({ name: 'FirstRaw' }).data(), Buffer.from('one'));
  const second = rawStreamFrame(fakeHeader({ name: 'SecondRaw' }).data(), Buffer.from('two'));
  socket.emitRaw('multiple-peer', [Buffer.concat([first, second])]);
  await waitForReceive(socket);
  await waitForCondition(() => events.length === 2, 'multiple stream dispatch');

  assert.deepEqual(events, [
    ['FirstRaw', 'one'],
    ['SecondRaw', 'two']
  ]);
  await runtime.dispose();
});

test('stream receive loop yields between bounded batches when Application HWM is unlimited', async () => {
  const socket = new FakeStreamSocket();
  const budget = new framework.ZLinkInboundDispatchBudget(0n);
  const totalFrames = 256;
  let dispatches = 0;
  for (let index = 0; index < totalFrames; index += 1) {
    socket.emitRaw('unlimited-peer', [
      rawStreamFrame(fakeHeader({ name: `Unlimited${index}` }).data(), Buffer.from('payload'))
    ]);
  }
  const runtime = createStreamRuntime({
    socket,
    inboundDispatchBudget: budget,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          dispatches += 1;
        }
      };
    }
  });

  let observerRan = false;
  const observer = new Promise((resolve) => {
    setImmediate(() => {
      observerRan = true;
      resolve();
    });
  });
  runtime.start();
  await observer;
  assert.equal(observerRan, true);
  assert.equal(socket.received.length > 0, true);

  await waitForCondition(() => dispatches === totalFrames, 'unlimited stream dispatch');
  await runtime.dispose();
});

test('stream session node runtime preserves multipart raw parts while assembling a frame', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push([header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  const frame = rawStreamFrame(
    fakeHeader({ name: 'Multipart' }).data(),
    Buffer.from('multipart-payload')
  );
  socket.emitRaw('multipart-peer', [frame.subarray(0, 6), frame.subarray(6)]);
  await waitForReceive(socket);
  await waitForCondition(() => events.length === 1, 'multipart stream dispatch');

  assert.deepEqual(events, [['Multipart', 'multipart-payload']]);
  await runtime.dispose();
});

test('stream session node runtime checks Poller readiness before Framework recv', async () => {
  const socket = new FakeStreamSocket();
  const waits = [];
  let ready = false;
  let disposed = false;
  const runtime = createStreamRuntime({
    socket,
    readablePoller: {
      wait(timeoutMs) {
        waits.push(timeoutMs);
        return ready;
      },
      dispose() {
        disposed = true;
      }
    },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          assert.equal(header.packetName, 'PollerReady');
          assert.equal(payload.decode(), 'payload');
        }
      };
    }
  });

  runtime.start();
  await waitForCondition(() => waits.length > 0, 'stream Poller probe');
  assert.deepEqual([...new Set(waits)], [0]);
  assert.equal(socket.recvCalls, 0);

  ready = true;
  socket.emitRaw('poller-peer', [
    rawStreamFrame(fakeHeader({ name: 'PollerReady' }).data(), Buffer.from('payload'))
  ]);
  await waitForCondition(() => socket.recvCalls > 0, 'Poller-gated stream recv');
  await runtime.dispose();

  assert.equal(disposed, true);
});

test('stream session node runtime stops recv at the host application HWM and resumes after dispatch', async () => {
  const socket = new FakeStreamSocket();
  const budget = new framework.ZLinkInboundDispatchBudget(3n);
  const events = [];
  let releaseFirst;
  const firstCanFinish = new Promise((resolve) => { releaseFirst = resolve; });
  let firstStarted;
  const firstStartedPromise = new Promise((resolve) => { firstStarted = resolve; });
  const runtime = createStreamRuntime({
    socket,
    inboundDispatchBudget: budget,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['start', header.packetName, payload.decode()]);
          if (header.packetName === 'BudgetFirst') {
            firstStarted();
            await firstCanFinish;
          }
          events.push(['end', header.packetName]);
        }
      };
    }
  });

  runtime.start();
  socket.emitRaw('budget-peer', [
    rawStreamFrame(fakeHeader({ name: 'BudgetFirst' }).data(), Buffer.from('one'))
  ]);
  await waitForReceive(socket);
  await firstStartedPromise;
  assert.equal(budget.receivePaused, true);

  const recvCallsWhilePaused = socket.recvCalls;
  socket.emitRaw('budget-peer', [
    rawStreamFrame(fakeHeader({ name: 'BudgetSecond' }).data(), Buffer.from('two'))
  ]);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(socket.recvCalls, recvCallsWhilePaused);
  assert.equal(socket.received.length, 1);

  releaseFirst();
  await waitForCondition(() => events.length === 4, 'resumed stream dispatch');
  assert.deepEqual(events, [
    ['start', 'BudgetFirst', 'one'],
    ['end', 'BudgetFirst'],
    ['start', 'BudgetSecond', 'two'],
    ['end', 'BudgetSecond']
  ]);
  assert.equal(budget.pendingPayloadBytes, 0n);
  await runtime.dispose();
});

test('stream session node runtime bounds zero-byte dispatch metadata independently of byte HWM', async () => {
  const socket = new FakeStreamSocket();
  const totalFrames = streamDispatchCapacity.ZLINK_STREAM_MAX_IN_FLIGHT_DISPATCHES + 1;
  let releaseFirst;
  const firstCanFinish = new Promise((resolve) => { releaseFirst = resolve; });
  let firstStarted;
  const firstStartedPromise = new Promise((resolve) => { firstStarted = resolve; });
  let dispatchCount = 0;
  const runtime = createStreamRuntime({
    socket,
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          dispatchCount += 1;
          if (dispatchCount === 1) {
            firstStarted();
            await firstCanFinish;
          }
        }
      };
    }
  });

  runtime.start();
  const frame = rawStreamFrame(
    fakeHeader({ name: 'ZeroByteDispatch' }).data(),
    Buffer.alloc(0)
  );
  socket.emitRaw('zero-byte-peer', [frame]);
  await waitForReceive(socket);
  await firstStartedPromise;

  for (let index = 1; index < totalFrames; index += 1) {
    socket.emitRaw('zero-byte-peer', [frame]);
  }
  await waitForCondition(
    () => socket.recvCalls >= streamDispatchCapacity.ZLINK_STREAM_MAX_IN_FLIGHT_DISPATCHES
      && socket.received.length > 0,
    'zero-byte dispatch capacity'
  );
  const recvCallsAtCapacity = socket.recvCalls;
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(dispatchCount, 1);
  assert.equal(socket.recvCalls, recvCallsAtCapacity);
  assert.equal(socket.received.length, 1);

  releaseFirst();
  await waitForCondition(
    () => dispatchCount === totalFrames && socket.received.length === 0,
    'zero-byte dispatch resume'
  );
  await runtime.dispose();
});

test('stream HWM does not block the final recv of a segmented frame', async () => {
  const socket = new FakeStreamSocket();
  const budget = new framework.ZLinkInboundDispatchBudget(3n);
  let releaseDispatch;
  const dispatchCanFinish = new Promise((resolve) => { releaseDispatch = resolve; });
  let dispatchStarted;
  const dispatchStartedPromise = new Promise((resolve) => { dispatchStarted = resolve; });
  const runtime = createStreamRuntime({
    socket,
    inboundDispatchBudget: budget,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          dispatchStarted();
          await dispatchCanFinish;
        }
      };
    }
  });

  runtime.start();
  const frame = rawStreamFrame(
    fakeHeader({ name: 'SegmentedHwm' }).data(),
    Buffer.from('payload')
  );
  const headerEnd = 6 + frame.readUInt16BE(0);
  socket.emitRaw('segmented-hwm-peer', [frame.subarray(0, headerEnd)]);
  await waitForReceive(socket);
  assert.equal(budget.receivePaused, false);

  socket.emitRaw('segmented-hwm-peer', [frame.subarray(headerEnd)]);
  await waitForReceive(socket);
  await dispatchStartedPromise;
  assert.equal(budget.receivePaused, true);

  releaseDispatch();
  await waitForCondition(() => budget.pendingPayloadBytes === 0n, 'segmented HWM dispatch');
  await runtime.dispose();
});

test('stream liveness deadlines pause while the host application HWM blocks receive', async () => {
  const socket = new FakeStreamSocket();
  const clock = new FakeLivenessClock();
  const budget = new framework.ZLinkInboundDispatchBudget(3n);
  let releaseDispatch;
  const dispatchCanFinish = new Promise((resolve) => { releaseDispatch = resolve; });
  let dispatchStarted;
  const dispatchStartedPromise = new Promise((resolve) => { dispatchStarted = resolve; });
  const runtime = createStreamRuntime({
    socket,
    livenessClock: clock,
    inboundDispatchBudget: budget,
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          dispatchStarted();
          await dispatchCanFinish;
        }
      };
    }
  });

  runtime.start();
  runtime.markConnected('paused-heartbeat-peer');
  await clock.flush();
  await clock.advance(1000);
  socket.emitRaw('paused-heartbeat-peer', [
    rawStreamFrame(fakeHeader({ name: 'Blocked' }).data(), Buffer.from('one'))
  ]);
  await waitForReceive(socket);
  await dispatchStartedPromise;
  assert.equal(budget.receivePaused, true);

  await clock.advance(6000);
  assert.deepEqual(socket.disconnects, []);

  releaseDispatch();
  await waitForCondition(() => budget.pendingPayloadBytes === 0n, 'paused stream resume');
  socket.emitRaw('paused-heartbeat-peer', [
    rawStreamFrame(fakeHeader({
      kind: connector.ZlinkStreamMessageKind.Control,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: '$zlink.heartbeat.pong'
    }).data(), Buffer.alloc(0))
  ]);
  await waitForReceive(socket);
  assert.deepEqual(socket.disconnects, []);
  await runtime.dispose();
});

test('stream session node runtime isolates a malformed peer from another peer', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['dispatch', context.sessionId, header.packetName, payload.decode()]);
        },
        async onDisconnected(ctx) {
          events.push(['disconnected', ctx.sessionId]);
        }
      };
    }
  });

  runtime.start();
  socket.emitRaw('malformed-peer', [rawStreamFrame(Buffer.from([0xff]), Buffer.alloc(0))]);
  await waitForReceive(socket);
  socket.emitRaw('healthy-peer', [
    rawStreamFrame(fakeHeader({ name: 'Healthy' }).data(), Buffer.from('ok'))
  ]);
  await waitForReceive(socket);
  await waitForCondition(() => events.some((event) => event[0] === 'dispatch'), 'healthy stream dispatch');

  assert.deepEqual(socket.disconnects, ['malformed-peer']);
  assert.deepEqual(events, [
    ['disconnected', 'malformed-peer'],
    ['dispatch', 'healthy-peer', 'Healthy', 'ok']
  ]);
  await runtime.dispose();
});

test('stream receive loop survives a recv error and a peer session factory error', async () => {
  const socket = new FakeStreamSocket();
  const errors = [];
  const events = [];
  let failNextRecv = true;
  const recv = socket.recv.bind(socket);
  socket.recv = () => {
    if (failNextRecv) {
      failNextRecv = false;
      throw new Error('transient recv failure');
    }
    return recv();
  };
  const runtime = createStreamRuntime({
    socket,
    onError(error) {
      errors.push(error.message);
    },
    headerDecoder: (header) => ({ name: header.getString() }),
    sessionFactory(context) {
      if (context.sessionId === 'factory-error-peer') {
        throw new Error('session factory failure');
      }
      return {
        context,
        async onDispatch(header, payload) {
          events.push([header.packetName, payload.decode()]);
        },
        async onDisconnected() {}
      };
    }
  });

  runtime.start();
  await waitForCondition(() => errors.includes('transient recv failure'), 'recv error report');
  socket.emitRaw('factory-error-peer', [
    rawStreamFrame(fakeHeader({ name: 'Rejected' }).data(), Buffer.from('bad'))
  ]);
  await waitForCondition(() => socket.disconnects.includes('factory-error-peer'), 'factory error isolation');
  socket.emitRaw('healthy-after-error-peer', [
    rawStreamFrame(fakeHeader({ name: 'HealthyAfterError' }).data(), Buffer.from('ok'))
  ]);
  await waitForCondition(() => events.length === 1, 'recv loop recovery dispatch');
  await runtime.dispose();

  assert.deepEqual(events, [['HealthyAfterError', 'ok']]);
  assert.deepEqual(socket.disconnects, ['factory-error-peer']);
  assert.deepEqual(errors, ['transient recv failure', 'session factory failure']);
});

test('stream session runtime rejects sessions that do not expose provided context', () => {
  const socket = new FakeStreamSocket();

  assert.throws(
    () => new framework.ZLinkStreamSessionRuntime({
      socket,
      sessionFactory() {
        return { context: {} };
      }
    }, 'session-b'),
    /provided by the stream runtime/
  );
});

test('stream session cleanup removes actor bindings without closing the stream again', async () => {
  const socket = new FakeStreamSocket();
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime();
  const runtime = new framework.ZLinkStreamSessionRuntime({
    socket,
    bindingRuntime,
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          await ctx.actors.bind({
            nodeRid: 'node-a',
            actorId: 'actor-a',
            objectGeneration: 1n,
            meshName: 'session-test'
          });
        },
        async onDisconnected() {}
      };
    }
  }, 'session-c');

  runtime.enqueueConnected();
  await runtime.dispose();

  assert.equal(bindingRuntime.find('actor-a'), undefined);
  assert.equal(socket.disconnects.length, 0);
});

test('stream session onDisconnected can explicitly notify bound actors before cleanup', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime({
    async notifyDisconnected(actor) {
      events.push(['actor-disconnected', actor.actorId]);
    }
  });
  const runtime = new framework.ZLinkStreamSessionRuntime({
    socket,
    bindingRuntime,
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          await ctx.actors.bind({ nodeRid: 'node-a', actorId: 'actor-a', generation: 1 });
        },
        async onDisconnected(ctx) {
          await ctx.actors.bound[0].notifyDisconnected();
          events.push(['session-disconnected']);
        }
      };
    }
  }, 'session-disconnect');

  runtime.enqueueConnected();
  runtime.enqueueDisconnected();
  await runtime.dispose();

  assert.deepEqual(events, [
    ['actor-disconnected', 'actor-a'],
    ['session-disconnected']
  ]);
  assert.equal(bindingRuntime.find('actor-a'), undefined);
});

test('physical stream disconnect automatically notifies every captured actor and always cleans up', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime({
    async notifyDisconnected(actor) {
      events.push(actor.actorId);
      if (actor.actorId === 'actor-a') {
        throw new Error('actor-a callback failed');
      }
    }
  });
  const runtime = new framework.ZLinkStreamSessionRuntime({
    socket,
    bindingRuntime,
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          await ctx.actors.bind({
            nodeRid: 'node-a',
            actorId: 'actor-a',
            objectGeneration: 1n,
            meshName: 'session-test'
          });
          await ctx.actors.bind({
            nodeRid: 'node-b',
            actorId: 'actor-b',
            objectGeneration: 1n,
            meshName: 'session-test'
          });
        },
        async onDisconnected() {}
      };
    }
  }, 'session-auto-disconnect');

  runtime.enqueueConnected();
  runtime.enqueueDisconnected();
  await runtime.dispose();

  assert.deepEqual(events.sort(), ['actor-a', 'actor-b']);
  assert.equal(bindingRuntime.find('actor-a'), undefined);
  assert.equal(bindingRuntime.find('actor-b'), undefined);
});

test('physical stream disconnect bounds a stalled actor callback by the lifecycle deadline', async () => {
  const socket = new FakeStreamSocket();
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime({
    actorBindTimeoutMs: 5,
    async notifyDisconnected() {
      await new Promise(() => {});
    }
  });
  const runtime = new framework.ZLinkStreamSessionRuntime({
    socket,
    bindingRuntime,
    sessionFactory(context) {
      return {
        context,
        async onConnected(ctx) {
          await ctx.actors.bind({
            nodeRid: 'node-a',
            actorId: 'actor-stalled',
            objectGeneration: 1n,
            meshName: 'session-test'
          });
        },
        async onDisconnected() {}
      };
    }
  }, 'session-stalled-disconnect');

  runtime.enqueueConnected();
  runtime.enqueueDisconnected();
  await runtime.dispose();

  assert.equal(bindingRuntime.find('actor-stalled'), undefined);
});

test('stream session node runtime closes rejected packets after dispose', async () => {
  const socket = new FakeStreamSocket();
  const runtime = createStreamRuntime({
    socket,
    sessionFactory(context) {
      return { context };
    }
  });
  const header = fakeMessage('h');
  const payload = fakeMessage('p');

  runtime.start();
  await runtime.dispose();
  socket.emitPacket('session-d', header, payload);

  assert.equal(header.closed, true);
  assert.equal(payload.closed, true);
});

test('stream session runtime replies to dispatch errors without session onError callback', async () => {
  const socket = new FakeStreamSocket();
  const errors = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    onError(error) {
      errors.push(['sink', error.message]);
    },
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          throw new Error('dispatch failed');
        },
        async onError(_context, error) {
          errors.push(['session', error.error]);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-e', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Request,
    requestSeq: 7n,
    name: 'Move'
  }), fakeMessage('p'));
  await waitForReceive(socket);
  await runtime.dispose();

  assert.deepEqual(errors, [['sink', 'dispatch failed']]);
  assert.equal(socket.sent.length, 1);
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(socket.sent[0].payload.data());
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Error);
  assert.equal(header.requestSeq, 7n);
  assert.equal(header.name, '');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), {
    code: 'Error',
    message: 'dispatch failed'
  });
});

test('stream session runtime encodes Framework error kinds as string error codes', async () => {
  const socket = new FakeStreamSocket();
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          throw new framework.ZLinkFrameworkException(
            framework.ZLinkFrameworkErrorKind.NotFound,
            'spot route is not ready'
          );
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-framework-error', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Request,
    requestSeq: 8n,
    name: 'Probe'
  }), fakeMessage('p'));
  await waitForReceive(socket);
  await runtime.dispose();

  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(socket.sent[0].payload.data());
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), {
    code: 'NotFound',
    message: 'spot route is not ready'
  });
});

test('stream session runtime replies with the admission error when a new request is sealed', async () => {
  const socket = new FakeStreamSocket();
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    claimApplicationWork() {
      throw new framework.ZLinkFrameworkException(
        framework.ZLinkFrameworkErrorKind.ShuttingDown,
        'STREAM dispatch was rejected because the framework is draining.'
      );
    },
    sessionFactory(context) {
      return { context };
    }
  });

  runtime.start();
  socket.emitPacket('session-sealed', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Request,
    requestSeq: 10n,
    name: 'Probe'
  }), fakeMessage('p'));
  await waitForReceive(socket);
  await runtime.dispose();

  assert.equal(socket.sent.length, 1);
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(socket.sent[0].payload.data());
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Error);
  assert.equal(header.requestSeq, 10n);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), {
    code: 'ShuttingDown',
    message: 'STREAM dispatch was rejected because the framework is draining.'
  });
});

test('stream session runtime keeps request streams open after route disconnect error replies', async () => {
  const socket = new FakeStreamSocket();
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    sessionFactory(context) {
      return {
        context,
        async onDispatch() {
          throw new framework.ZLinkRouteDisconnectedError('yield.spot.route', 512, 4);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-route-error', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Request,
    requestSeq: 9n,
    name: 'YieldShutdownScenarioReq'
  }), fakeMessage('p'));
  await waitForReceive(socket);
  await runtime.dispose();

  assert.deepEqual(socket.disconnects, []);
  assert.equal(socket.sent.length, 1);
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(socket.sent[0].payload.data());
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Error);
  assert.equal(header.requestSeq, 9n);
  assert.equal(header.name, '');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), {
    code: 'ZLinkRouteDisconnectedError',
    message: 'yield.spot.route'
  });
});

test('stream session runtime completes pending responses before session dispatch', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  let pending;
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    sessionFactory(context) {
      pending = context.startRequest(1000);
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['dispatch', header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-f', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Response,
    requestSeq: 1n,
    name: 'Move'
  }), fakeMessage('response-body'));

  await waitForCondition(() => pending !== undefined, 'pending stream request');
  const response = await pending.promise;
  await runtime.dispose();

  assert.equal(response.getString(), 'response-body');
  assert.deepEqual(events, []);
});

test('stream session runtime decompresses response frames before completing pending requests', async () => {
  const socket = new FakeStreamSocket();
  let pending;
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    sessionFactory(context) {
      pending = context.startRequest(1000);
      return { context };
    }
  });

  runtime.start();
  socket.emitPacket('session-compressed-response', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Response,
    flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
    requestSeq: 1n,
    name: 'Move'
  }), fakeMessageBytes(Buffer.from('40551F41010047504141414141', 'hex')));

  await waitForCondition(() => pending !== undefined, 'compressed pending stream request');
  const response = await pending.promise;
  await runtime.dispose();

  assert.equal(response.getString(), 'A'.repeat(96));
});

test('stream session runtime decompresses dispatch payloads before session handlers', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['dispatch', header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-compressed-dispatch', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Send,
    flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
    name: 'Move'
  }), fakeMessageBytes(Buffer.from('40551F41010047504141414141', 'hex')));
  await waitForReceive(socket);
  await runtime.dispose();

  assert.deepEqual(events, [['dispatch', 'Move', 'A'.repeat(96)]]);
});

test('stream session runtime rejects compressed dispatch payloads above receive limit', async () => {
  const socket = new FakeStreamSocket();
  // Keep the transport frame below its own limit so this test reaches the
  // separate decompressed-payload guard.
  socket.maxMessageSize = 128 * 1024;
  const errors = [];
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    onError(error) {
      errors.push(error);
    },
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['dispatch', header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-compressed-too-large', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Send,
    flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
    name: 'Move'
  }), fakeMessageBytes(Uint8Array.from([0x40, 0x01, ...Buffer.from('A'.repeat(64 * 1024), 'utf8')])));
  await waitForReceive(socket);
  await runtime.dispose();

  assert.deepEqual(events, []);
  assert.equal(errors.length, 1);
  assert.match(errors[0].message, /maximum stream payload size/);
});

test('stream session pending request timeout removes request sequence', async () => {
  const context = new framework.ZLinkStreamBindingRuntime().createSessionContext({
    sessionId: 'session-timeout',
    routingId: 'session-timeout',
    write() {
      return true;
    },
    async close() {}
  });
  const pending = context.startRequest(1);
  await assert.rejects(
    () => pending.promise,
    /Client stream request timed out/
  );

  const consumed = context.tryCompleteResponse({
    kind: 3,
    codec: 1,
    flags: 1,
    requestSeq: pending.requestSeq,
    name: 'LateReply',
    metadata: new Map()
  }, fakeMessage('late-body'));

  assert.equal(consumed, false);
});

test('stream session runtime dispatches unmatched response frames to the session', async () => {
  const socket = new FakeStreamSocket();
  const events = [];
  const runtime = createStreamRuntime({
    socket,
    headerDecoder: (header) => JSON.parse(header.getString(), streamHeaderReviver),
    sessionFactory(context) {
      return {
        context,
        async onDispatch(header, payload) {
          events.push(['dispatch', header.packetName, payload.decode()]);
        }
      };
    }
  });

  runtime.start();
  socket.emitPacket('session-g', fakeHeader({
    kind: connector.ZlinkStreamMessageKind.Response,
    requestSeq: 99n,
    name: 'Move'
  }), fakeMessage('unmatched'));
  await waitForReceive(socket);
  await runtime.dispose();

  assert.deepEqual(events, [['dispatch', '', 'unmatched']]);
});

test('stream session node runtime receives framed packets from public binding stream socket', async () => {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const streamAdapter = factory.createStreamAdapter();
  const socket = streamAdapter.createStreamSocket(context);
  let client;
  let runtime;
  let resolveDispatched;
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: {
      createTextMessage(payload) {
        return zlink.Message.from(Buffer.from(payload));
      },
      createBinaryMessage(payload) {
        return zlink.Message.from(Buffer.from(payload));
      }
    }
  });

  const dispatched = new Promise((resolve) => {
    resolveDispatched = resolve;
  });

  try {
    socket.bind(endpoint);
    runtime = createStreamRuntime({
      socket,
      readablePoller: streamAdapter.createReadablePoller(socket),
      bindingRuntime,
      headerDecoder: (header) => protocolCodecs.ZlinkStreamHeaderCodec.decode(header.data()),
      sessionFactory(sessionContext) {
        return {
          context: sessionContext,
          async onDispatch(header, payload) {
            resolveDispatched({
              sessionId: sessionContext.sessionId,
              routingId: sessionContext.routingId,
              header: header.packetName,
              payload: payload.decode()
            });
            await sessionContext.client.reply('NativeReply').submit();
          }
        };
      }
    });
    runtime.start();
    client = net.createConnection({ host: '127.0.0.1', port });
    await once(client, 'connect');
    const response = once(client, 'data').then(([chunk]) => chunk);
    const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Request,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq: 7n,
      name: 'NativeHeader',
      metadata: connector.ZlinkStreamMetadataMap.empty
    });
    client.write(protocolCodecs.ZlinkStreamFrameCodec.encode(
      requestHeader,
      new TextEncoder().encode('"NativePayload"')
    ));

    const received = await withTimeout(dispatched, 1000, 'native stream session dispatch');
    assert.equal(received.header, 'NativeHeader');
    assert.equal(received.payload, 'NativePayload');
    assert.equal(typeof received.sessionId, 'string');
    assert.equal(received.sessionId.length > 0, true);
    assert.equal(received.routingId, received.sessionId);
    const responseFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(
      await withTimeout(response, 1000, 'native stream session reply')
    );
    const responseHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(responseFrame.header);
    assert.equal(responseHeader.kind, connector.ZlinkStreamMessageKind.Response);
    assert.equal(responseHeader.requestSeq, 7n);
    assert.equal(new TextDecoder().decode(responseFrame.payload), '"NativeReply"');
  } finally {
    await closeClient(client);
    await runtime?.dispose();
    await socket.dispose();
    await context.dispose();
  }
});

function createStreamRuntime(options) {
  return new framework.ZLinkStreamSessionNodeRuntime({
    readablePoller: readyPoller(),
    ...options
  });
}

class FakeStreamSocket {
  constructor() {
    this.disconnects = [];
    this.sent = [];
    this.received = [];
    this.recvCalls = 0;
    this.sendReadyHandler = undefined;
    // ZLinkBackendStreamSocket declares sendTimeoutMs and sendHighWaterMark as
    // required numbers. The runtime derives its outbound admission budget from
    // them, so a fake that omits them is not a backend socket.
    this.sendTimeoutMs = 0;
    this.sendHighWaterMark = 0;
    this.maxMessageSize = 64 * 1024;
  }

  recv() {
    this.recvCalls += 1;
    return this.received.shift();
  }

  onSendReady(handler) {
    this.sendReadyHandler = handler;
  }

  send(routingId, payload, flags) {
    this.sent.push({ routingId, payload, flags });
    return true;
  }

  disconnectPeer(routingId) {
    this.disconnects.push(routingId);
  }

  emitPacket(routingId, header, payload) {
    const frame = rawStreamFrame(
      header.data(),
      payload.data()
    );
    header.close();
    payload.close();
    this.received.push(receivedEnvelope(routingId, [zlink.Message.from(frame)]));
  }

  emitRaw(routingId, parts) {
    this.received.push(receivedEnvelope(routingId, parts.map((part) => zlink.Message.from(part))));
  }

  async dispose() {}
  async bindActor() {}
  async unbindActor() {}
  sendBoundActor() { return true; }
}

function readyPoller() {
  return {
    wait() { return true; },
    dispose() {}
  };
}

class FakeLivenessClock {
  constructor() {
    this.current = 0;
    this.nextTimer = 1;
    this.timers = new Map();
  }

  now = () => this.current;

  setTimer = (callback, delayMs) => {
    const id = this.nextTimer++;
    this.timers.set(id, { callback, due: this.current + delayMs });
    return id;
  };

  clearTimer = (id) => {
    this.timers.delete(id);
  };

  async advance(delayMs) {
    const target = this.current + delayMs;
    for (;;) {
      const next = [...this.timers.entries()]
        .filter(([, timer]) => timer.due <= target)
        .sort((left, right) => left[1].due - right[1].due)[0];
      if (next === undefined) break;
      const [id, timer] = next;
      this.timers.delete(id);
      this.current = timer.due;
      timer.callback();
      await this.flush();
    }
    this.current = target;
    await this.flush();
  }

  async flush() {
    await new Promise((resolve) => setImmediate(resolve));
  }
}

function controlHeader(sent) {
  return decodeSessionClosing(sent).header;
}

function decodeSessionClosing(sent) {
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(sent.payload.data());
  return {
    header: protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header),
    payload: frame.payload
  };
}

function fakeMessage(text) {
  const payload = Buffer.from(text);
  return {
    closed: false,
    data() {
      return payload;
    },
    size() {
      return payload.length;
    },
    toBytes() {
      return new Uint8Array(payload);
    },
    getString() {
      return text;
    },
    close() {
      this.closed = true;
    }
  };
}

function fakeMessageBytes(bytes) {
  const payload = new Uint8Array(bytes);
  return {
    closed: false,
    bytes: payload,
    toBytes() {
      return new Uint8Array(payload);
    },
    data() {
      return payload;
    },
    size() {
      return payload.byteLength;
    },
    getString() {
      return new TextDecoder().decode(payload);
    },
    close() {
      this.closed = true;
    }
  };
}

async function reservePort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { port } = server.address();
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function closeClient(client) {
  if (client === undefined) {
    return;
  }
  if (client.destroyed) {
    return;
  }
  const closed = once(client, 'close');
  client.destroy();
  await closed;
}

function withTimeout(promise, timeoutMs, label) {
  let timeout;
  const guard = new Promise((_, reject) => {
    timeout = setTimeout(() => reject(new Error(`${label} timed out`)), timeoutMs);
  });
  return Promise.race([promise, guard]).finally(() => clearTimeout(timeout));
}

async function waitForReceive(socket) {
  await waitForCondition(() => socket.received.length === 0, 'stream recv');
  await new Promise((resolve) => setImmediate(resolve));
}

async function waitForCondition(predicate, label, timeoutMs = 1000) {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() >= deadline) {
      throw new Error(`${label} timed out`);
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
}

function fakeHeader(overrides) {
  return fakeMessageBytes(streamProtocol.encodeStreamHeader(streamHeader(overrides)));
}

function rawStreamFrame(header, payload) {
  const headerBytes = Buffer.from(header);
  const payloadBytes = Buffer.from(payload);
  const frame = Buffer.alloc(6 + headerBytes.length + payloadBytes.length);
  frame.writeUInt16BE(headerBytes.length, 0);
  frame.writeUInt32BE(payloadBytes.length, 2);
  headerBytes.copy(frame, 6);
  payloadBytes.copy(frame, 6 + headerBytes.length);
  return frame;
}

function receivedEnvelope(routingId, parts) {
  return {
    routingId,
    parts,
    close() {
      for (const part of parts) part.close();
    }
  };
}

function streamHeader(overrides) {
  return {
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: overrides.requestSeq === undefined
      ? connector.ZlinkStreamHeaderFlags.None
      : connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    name: 'Packet',
    metadata: new Map(),
    ...overrides
  };
}

function streamHeaderReviver(key, value) {
  if (key === 'requestSeq' && typeof value === 'string') {
    return BigInt(value);
  }
  if (key === 'metadata' && value?.values !== undefined) {
    return { values: new Map(value.values) };
  }
  return value;
}
