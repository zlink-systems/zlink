'use strict';

const assert = require('node:assert/strict');
const { EventEmitter, once } = require('node:events');
const { appendFileSync } = require('node:fs');
const { setImmediate: nextTurn } = require('node:timers/promises');
const test = require('node:test');
const { BroadcastChannel } = require('node:worker_threads');
const binding = require('@zlink-systems/zlink');
const { logs, SeverityNumber } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkTerminationOutcome,
  ZLinkFrameworkTerminationReason
} = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const connector = require('../../packages/stream-connector/dist');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

test(
  'STREAM session factory failure replies, logs, and closes only its connection',
  { timeout: 15000 },
  async (t) => {
    const nodeName = 'creation-failure';
    const packetName = 'FirstRequest';
    const exception = new TypeError('session factory refused this connection');
    const diagnostics = [];
    const flowEvents = [];
    const telemetryReady = deferred();
    const telemetry = new EventEmitter();
    const channelName = `zlink-session-creation-${process.pid}`;
    const previousOptions = process.env.NODE_OPTIONS;
    const previousChannel = process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL;
    const preload = require.resolve('./fixtures/message-flow-provider.cjs').replace(/\\/g, '/');
    process.env.NODE_OPTIONS = `${previousOptions ?? ''} --require "${preload}"`;
    process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL = channelName;
    const channel = new BroadcastChannel(channelName);
    channel.onmessage = ({ data }) => {
      if (data.kind === 'ready') telemetryReady.resolve();
      if (data.kind === 'log' && data.record.attributes.packet_name === packetName) {
        flowEvents.push(data.record.attributes);
        if (data.record.attributes.outcome === 'failed') telemetry.emit('failed');
      }
    };
    const logger = new LoggerProvider({
      processors: [{
        onEmit(record) {
          diagnostics.push(record);
          if (process.env.ZLINK_TEST_SESSION_CREATION_LOG) {
            const { eventName, severityNumber, body, attributes } = record;
            appendFileSync(process.env.ZLINK_TEST_SESSION_CREATION_LOG,
              `${JSON.stringify({ eventName, severityNumber, body, attributes })}\n`);
          }
        },
        forceFlush() { return Promise.resolve(); },
        shutdown() { return Promise.resolve(); }
      }]
    });
    logs.setGlobalLoggerProvider(logger);
    t.after(async () => {
      channel.close();
      logs.disable();
      await logger.shutdown();
      if (previousOptions === undefined) delete process.env.NODE_OPTIONS;
      else process.env.NODE_OPTIONS = previousOptions;
      if (previousChannel === undefined) delete process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL;
      else process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL = previousChannel;
    });

    const factoryAttempts = [];
    const disconnects = [];
    let monitorPaused = false;
    const createStreamSocket = binding.createStreamSocket;
    let monitorCount = 0;
    // Defer observation through the server's existing public binding monitor.
    // The native socket queues the original events in their original order.
    t.mock.method(binding, 'createStreamSocket', function (...args) {
      const socket = createStreamSocket.apply(this, args);
      const monitorOpen = socket.monitorOpen;
      t.mock.method(socket, 'monitorOpen', function (...openArgs) {
        const monitor = monitorOpen.apply(this, openArgs);
        const recv = monitor.recv;
        monitorCount++;
        t.mock.method(monitor, 'recv', function (...recvArgs) {
          if (monitorPaused) return null;
          const event = recv.apply(this, recvArgs);
          if (
            event?.event === binding.MonitorEventType.ConnectionReady &&
            (event.flags & binding.MonitorEventFlag.ConnectionReadyEdge) !== 0 &&
            event.routingId?.toHex() === factoryAttempts[1]
          ) {
            telemetry.emit('failedPeerReady');
          }
          return event;
        });
        return monitor;
      });
      return socket;
    });
    class SessionFactory {
      async create(context) {
        factoryAttempts.push(context.sessionId);
        if (factoryAttempts.length === 2) throw exception;
        return {
          context,
          async onDispatch({ packetName: name }) {
            assert.equal(name, 'Echo');
            await context.client.reply({ ok: true }).submit();
          },
          onDisconnected() {
            disconnects.push(context.sessionId);
          }
        };
      }
    }

    const builder = nest.zlinkFramework();
    builder.configureDispatch().messageFlow('normal');
    builder.addStreamNode(nodeName).bind('ws://127.0.0.1:*').registerSession(SessionFactory);
    class AppModule {}
    Module({ imports: [nest.ZLinkModule.forRoot(builder.build())] })(AppModule);
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    const host = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
    const endpoint = host.getListenerStatus('stream', nodeName).endpoint;
    const clients = [];
    t.after(async () => {
      monitorPaused = false;
      try {
        for (const client of clients) await client.close();
      } finally {
        await app.close();
      }
    });
    async function connect() {
      const client = connector.zlinkStreamConnectorFactory.create({
        endpoint,
        dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
        heartbeat: { enabled: false },
        reconnect: { enabled: false }
      });
      clients.push(client);
      await client.connect();
      return client;
    }
    const echo = (client) => client.request({}).packetName('Echo').timeout(2000).submit();
    const existing = await connect();
    assert.deepEqual(await echo(existing), { ok: true });
    await telemetryReady.promise;
    assert.equal(monitorCount, 1);

    monitorPaused = true;
    const failing = await connect();
    failing.onDisconnected(() => telemetry.emit('disconnected'));
    const deadline = AbortSignal.timeout(2000);
    const observedFailure = Promise.allSettled([
      once(telemetry, 'disconnected', { signal: deadline }),
      once(telemetry, 'failed', { signal: deadline }),
      assert.rejects(failing.request({}).packetName(packetName).timeout(2000).submit(),
        (error) => error.error?.code === connector.ZlinkStreamErrorCode.RemoteError &&
          error.error.cause?.code === 'internal_failure')
    ]);
    const outcomes = await observedFailure;
    const failures = [];
    for (const [index, step] of ['disconnect', 'flow outcome', 'InternalFailure reply'].entries()) {
      if (outcomes[index].status === 'rejected') failures.push(`${step}: ${outcomes[index].reason}`);
    }
    assert.deepEqual(failures, [], JSON.stringify(flowEvents));
    assert.equal(failing.closeReason, 'TransportError');
    assert.equal(failing.isConnected, false);
    const lateReady = once(telemetry, 'failedPeerReady', { signal: deadline });
    monitorPaused = false;
    await lateReady;
    assert.deepEqual(flowEvents.filter((event) => event.outcome === 'failed').map((event) => ({
      surface: event.surface,
      reason: event.reason,
      errorType: event.error_type,
      errorMessage: event.error_message
    })), [{
      surface: 'stream',
      reason: 'session_creation_failed',
      errorType: exception.name,
      errorMessage: exception.message
    }]);

    const errors = diagnostics.filter((record) =>
      record.severityNumber >= SeverityNumber.ERROR &&
      JSON.stringify([record.body, record.attributes]).includes(exception.message));
    assert.equal(errors.length, 1);
    assert.match(JSON.stringify([errors[0].body, errors[0].attributes]), /creation-failure/);
    assert.match(JSON.stringify([errors[0].body, errors[0].attributes]), /SessionFactory/);
    assert.deepEqual(await echo(existing), { ok: true });
    assert.deepEqual(await echo(await connect()), { ok: true });
    assert.equal(factoryAttempts.length, 3, JSON.stringify({ factoryAttempts, disconnects }));
  }
);

async function connectSession(t) {
  const entered = deferred();
  const release = deferred();
  const finished = deferred();
  const callbacks = [];

  class SessionFactory {
    async create(context) {
      return {
        context,
        async onConnected() {
          entered.resolve(context);
          await release.promise;
          callbacks.push('connected');
          finished.resolve();
        },
        async onDispatch({ packetName }) {
          callbacks.push(packetName);
        },
        async onDisconnected() {
          callbacks.push('disconnected');
        },
        async onError() {
          callbacks.push('error');
        }
      };
    }
  }

  const builder = nest.zlinkFramework();
  builder.configureDispatch().messageFlow('normal');
  builder.addStreamNode('probe').bind('ws://127.0.0.1:*').registerSession(SessionFactory);
  class AppModule {}
  Module({ imports: [nest.ZLinkModule.forRoot(builder.build())] })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  const host = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
  const client = connector.zlinkStreamConnectorFactory.create({
    endpoint: host.getListenerStatus('stream', 'probe').endpoint,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  });
  const disconnected = deferred();
  client.onDisconnected(disconnected.resolve);
  t.after(async () => {
    release.resolve();
    try {
      await client.close();
    } finally {
      await app.close();
    }
  });

  await client.connect();
  // An unaddressed WebSocket readiness event needs a packet to identify its session.
  await client.send({}).packetName('Prime').submit();
  const context = await entered.promise;
  assert.deepEqual(callbacks, callbacks.length === 0 ? [] : ['Prime']);
  const startupCallbacks = [...callbacks];
  await client.send({}).packetName('Probe').submit();
  return { host, context, release, finished, disconnected, callbacks, startupCallbacks };
}

test('STREAM-only host shuts down while onConnected is pending', { timeout: 15000 }, async (t) => {
  const { host, context, release, finished, disconnected, callbacks, startupCallbacks } =
    await connectSession(t);
  const deadlineMs = 2000;
  const startedAt = performance.now();
  let watchdog;
  try {
    const shutdown = host.shutdown({ deadlineMs });
    const result = await Promise.race([
      shutdown,
      new Promise((_, reject) => {
        watchdog = setTimeout(() => {
          const elapsedMs = Math.round(performance.now() - startedAt);
          reject(
            new Error(`Shutdown still pending after ${elapsedMs}ms with deadlineMs=${deadlineMs}`)
          );
        }, deadlineMs + 500);
      })
    ]);
    assert.deepEqual(result, {
      outcome: ZLinkFrameworkTerminationOutcome.ForceStopped,
      reason: ZLinkFrameworkTerminationReason.DeadlineExceeded
    });
    assert.deepEqual(callbacks, startupCallbacks);
  } finally {
    clearTimeout(watchdog);
  }

  await disconnected.promise;
  release.resolve();
  await finished.promise;
  class AfterShutdown {}
  await assert.rejects(context.client.send(new AfterShutdown()).submit(), {
    kind: ZLinkFrameworkErrorKind.ShuttingDown
  });
  await context.close();
  await nextTurn();
  assert.deepEqual(callbacks, [...startupCallbacks, 'connected']);
});

test(
  'STREAM-only host drains when onConnected finishes before the deadline',
  { timeout: 15000 },
  async (t) => {
    const { host, release, disconnected, callbacks, startupCallbacks } = await connectSession(t);
    const shutdown = host.shutdown({ deadlineMs: 2000 });
    release.resolve();
    assert.deepEqual(await shutdown, {
      outcome: ZLinkFrameworkTerminationOutcome.Stopped,
      reason: ZLinkFrameworkTerminationReason.None
    });
    await disconnected.promise;
    const drainedCallbacks = callbacks.slice(startupCallbacks.length);
    assert.equal(drainedCallbacks[0], 'connected');
    assert.equal(drainedCallbacks.at(-1), 'disconnected');
    assert.equal(callbacks.filter((callback) => callback === 'disconnected').length, 1);
  }
);
