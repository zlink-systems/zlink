const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate, setTimeout } = require('node:timers/promises');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const { zlinkStreamConnectorFactory } = require('../../packages/stream-connector/dist');
const { createBrowserSafeStreamApp } = require('./helpers/browser-safe-stream-app');

const testOptions = { timeout: 10000 };
const meshName = 'play';
const listenerName = 'gateway';
const deadlineMs = 250;
const meshDrain = 'host.route_mesh_drain';
const meshShutdown = ['host.mesh_shutdown', meshName];
const streamDrain = 'host.unscoped_stream_drain';

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

function heldCallback() {
  const entered = deferred();
  const released = deferred();
  return {
    entered: entered.promise,
    release: released.resolve,
    async wait() {
      entered.resolve();
      await released.promise;
    }
  };
}

function captureDeadlineDiagnostics(t, enabled = true) {
  const previous = process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
  process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = enabled ? '1' : '0';
  t.after(() => {
    if (previous === undefined) delete process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
    else process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = previous;
  });

  const records = [];
  const meshDrained = deferred();
  const originalError = console.error;
  t.mock.method(console, 'error', (...args) => {
    const [prefix, marker, detail] = args;
    if (prefix !== '[zlink.runtime.relocation]') return originalError(...args);
    if (marker === 'shutdown_deadline_exceeded') records.push(detail);
    if (
      marker === 'shutdown_step' &&
      detail.stage === 'host.cleanup_resources' &&
      detail.outcome === 'completed'
    ) {
      meshDrained.resolve();
    }
  });
  return { records, meshDrained: meshDrained.promise };
}

async function createApp(builder, providers = []) {
  class AppModule {}
  Module({ imports: [nest.ZLinkModule.forRoot(builder.build())], providers })(AppModule);
  return NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
}

async function assertDeadline(shutdown, records, stages) {
  assert.deepEqual(await shutdown, {
    outcome: framework.ZLinkFrameworkTerminationOutcome.ForceStopped,
    reason: framework.ZLinkFrameworkTerminationReason.DeadlineExceeded
  });
  if (stages.length === 0) {
    assert.deepEqual(records, []);
    return;
  }
  assert.equal(records.length, 1, 'expected one terminal deadline diagnostic');
  const { pendingSteps } = records[0];
  assert.deepEqual(
    pendingSteps.map(({ stage, meshName }) => [stage, meshName]),
    stages.map(([stage, meshName]) => [stage, meshName])
  );
  for (const { stage, elapsedMs } of pendingSteps) {
    assert.ok(elapsedMs > 0 && elapsedMs <= deadlineMs, `${stage}: elapsedMs=${elapsedMs}`);
  }
}

test(
  'shutdown deadline diagnostics include RouteMesh drain and Spot cleanup',
  testOptions,
  async (t) => {
    const closing = heldCallback();
    class Spot {
      async onClosing() {
        await closing.wait();
      }
    }
    Injectable({ scope: Scope.TRANSIENT })(Spot);
    const builder = nest.zlinkFramework().options({ locations: { useInMemoryStores: true } });
    builder
      .addRouteMesh(meshName)
      .listen('tcp://127.0.0.1:0')
      .setRoutingIdPrefix(meshName)
      .objects()
      .server()
      .addSpotFactory('room', Spot, (factory) => factory.disableRelocation());
    const app = await createApp(builder, [Spot]);
    try {
      await app.get(nest.ZLINK_SPOT_MANAGER).create('room').inMesh(meshName).submit();
      const { records } = captureDeadlineDiagnostics(t);
      const shutdown = app.get(nest.ZLINK_FRAMEWORK_RUNTIME).shutdown({ deadlineMs });
      await closing.entered;
      await setTimeout(deadlineMs);
      closing.release();
      await assertDeadline(shutdown, records, [
        [meshDrain],
        meshShutdown,
        ['owner_cleanup.spots', meshName]
      ]);
    } finally {
      closing.release();
      await app.close();
    }
  }
);

for (const scoped of [false, true]) {
  test(
    `shutdown deadline diagnostics include ${scoped ? 'mesh Session barrier' : 'unscoped Stream drain'}`,
    testOptions,
    async (t) => {
      const connected = deferred();
      const disconnected = heldCallback();
      class SessionFactory {
        async create(context) {
          return {
            context,
            async onConnected() {
              connected.resolve();
            },
            async onDisconnected() {
              await disconnected.wait();
            }
          };
        }
      }
      const builder = nest.zlinkFramework();
      const stream = builder
        .addStreamNode(listenerName)
        .bind('ws://127.0.0.1:0')
        .registerSession(SessionFactory);
      if (scoped) {
        builder.options({ locations: { useInMemoryStores: true } });
        builder.addRouteMesh(meshName).listen('tcp://127.0.0.1:0').objects().client();
        stream.enableActorDispatch();
      }
      const app = await createBrowserSafeStreamApp(
        () => createApp(builder, [SessionFactory]),
        listenerName
      );
      const runtime = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
      const { endpoint } = runtime.getListenerStatus('stream', listenerName);
      const client = zlinkStreamConnectorFactory.create({ endpoint });
      try {
        await client.connect();
        await connected.promise;
        const { records, meshDrained } = captureDeadlineDiagnostics(t);
        const shutdown = runtime.shutdown({ deadlineMs });
        await disconnected.entered;
        if (!scoped) {
          await meshDrained;
          // The coordinator returns after the resource-cleanup diagnostic.
          await setImmediate();
        }
        await setTimeout(deadlineMs);
        disconnected.release();
        await assertDeadline(
          shutdown,
          records,
          scoped
            ? [[meshDrain], meshShutdown, ['session_barrier.notify', meshName]]
            : [[streamDrain]]
        );
      } finally {
        disconnected.release();
        try {
          await client.close();
        } finally {
          await app.close();
        }
      }
    }
  );
}

async function unexpectedBlobOperation() {
  assert.fail('shutdown of an idle host must not access relocation blobs');
}

for (const mode of ['timer', 'late completion', 'disabled']) {
  test(`shutdown deadline diagnostics for host teardown (${mode})`, testOptions, async (t) => {
    const disposal = heldCallback();
    const store = {
      put: unexpectedBlobOperation,
      read: unexpectedBlobOperation,
      renew: unexpectedBlobOperation,
      delete: unexpectedBlobOperation,
      dispose: disposal.wait
    };
    const app = await createApp(nest.zlinkFramework().addRelocationStore(store));
    try {
      const enabled = mode !== 'disabled';
      const { records } = captureDeadlineDiagnostics(t, enabled);
      let clockOffset = 0;
      if (mode === 'late completion') {
        const monotonicNow = performance.now.bind(performance);
        t.mock.method(performance, 'now', () => monotonicNow() + clockOffset);
      }
      const shutdown = app.get(nest.ZLINK_FRAMEWORK_RUNTIME).shutdown({ deadlineMs });
      await disposal.entered;
      if (mode === 'late completion') clockOffset = deadlineMs + 1;
      else await setTimeout(deadlineMs);
      disposal.release();
      await assertDeadline(
        shutdown,
        records,
        enabled ? [['host.teardown'], ['owner_cleanup.store']] : []
      );
    } finally {
      disposal.release();
      await app.close();
    }
  });
}

test('shutdown deadline diagnostics omit a normal shutdown', testOptions, async (t) => {
  const app = await createApp(nest.zlinkFramework());
  const { records } = captureDeadlineDiagnostics(t);
  try {
    assert.deepEqual(await app.get(nest.ZLINK_FRAMEWORK_RUNTIME).shutdown(), {
      outcome: framework.ZLinkFrameworkTerminationOutcome.Stopped,
      reason: framework.ZLinkFrameworkTerminationReason.None
    });
    assert.deepEqual(records, []);
  } finally {
    await app.close();
  }
});
