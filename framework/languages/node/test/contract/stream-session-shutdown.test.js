'use strict';

const assert = require('node:assert/strict');
const { setImmediate: nextTurn } = require('node:timers/promises');
const test = require('node:test');
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
