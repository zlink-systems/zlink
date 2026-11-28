'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: nextTurn } = require('node:timers/promises');
const telemetry = require('./helpers/telemetry-log-capture');
const deferred = require('./helpers/deferred.cjs');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} = require('../../packages/framework/dist');
const nestjs = require('../../packages/nestjs/dist');
const connector = require('../../packages/stream-connector/dist');
const { createBrowserSafeStreamApp } = require('./helpers/browser-safe-stream-app');

const actorId = 'local-send-player';
const packetName = 'RelayNotice';

test('local session Actor SEND completes at FIFO admission', { timeout: 30000 }, async (t) => {
  const received = [];
  const relayResults = [];
  const runtimeErrors = [];
  const started = deferred();
  const release = deferred();
  let beforeRelay;
  let beforeLocalAdmission;
  let sessionStream;

  class Player {
    constructor(context) {
      this.context = context;
    }
  }
  class Factory {
    async create(context) {
      return new Player(context);
    }
  }
  class Entry {
    constructor(context) {
      this.context = context;
    }
    configure() {}
  }
  class Notice {
    async handle(_spot, _actor, _context, message) {
      received.push(message.value);
      if (message.value !== 'second' && message.value !== 'refused') {
        started.resolve();
        await release.promise;
      }
      if (message.value === 'throw') throw new Error('session Actor SEND handler failed');
    }
  }
  nestjs.zlinkEntrySpotActorSendHandler({
    entrySpot: () => Entry,
    actor: () => Player,
    packetName
  })(Notice);

  let actorRef;
  class SessionFactory {
    async create(context) {
      return {
        context,
        async onDispatch(dispatch, message) {
          if (dispatch.actor) {
            const terminal = relayResults.shift();
            try {
              if (beforeRelay) await beforeRelay();
              await dispatch.actor.relay(dispatch, message);
              terminal.resolve({ status: 'Submitted' });
            } catch (error) {
              terminal.resolve({ status: 'Rejected', error });
            }
            return;
          }
          await context.actors.bindOrGet(actorRef);
          sessionStream = context.stream;
          await context.client.reply({ bound: true }).submit();
        }
      };
    }
  }
  const builder = nestjs
    .zlinkFramework()
    .addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
  builder.configureDispatch().messageFlow('normal');
  builder
    .addRouteMesh('local-send')
    .listen('tcp://127.0.0.1:0')
    .objects()
    .server()
    .addEntrySpot(Entry, 'player')
    .addActorFactory('player', Factory, (factory) => factory.disableRelocation());
  builder
    .addStreamNode('clients')
    .bind('ws://127.0.0.1:*')
    .enableActorDispatch()
    .registerSession(SessionFactory);

  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [Factory, SessionFactory, Notice]
  })(AppModule);

  const app = await createBrowserSafeStreamApp(
    () => NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false }),
    'clients'
  );
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
  const unsubscribe = runtime.errorSink.onRuntimeTaskException((error) =>
    runtimeErrors.push(error)
  );
  const client = connector.zlinkStreamConnectorFactory.create({
    endpoint: runtime.getListenerStatus('stream', 'clients').endpoint,
    reconnect: { enabled: false },
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
  });
  t.after(async () => {
    release.resolve();
    unsubscribe();
    await client.close();
    await app.close();
    telemetry.close();
  });

  const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
  await actors.getOrCreate(actorId, 'player').inMesh('local-send').submit();
  actorRef = await actors.find(actorId);
  await client.connect();
  assert.deepEqual(await client.request({}).packetName('Bind').submit(), { bound: true });

  const entry = runtime.spotNodeRuntime.primaryEntryActivation();
  const scheduler = entry.spotSerialExecutor.actorExecutor(actorId).scheduler;
  const admit = scheduler.admit.bind(scheduler);
  let admissions = 0;
  scheduler.admit = (operation, options, ...args) =>
    admit(
      operation,
      {
        ...options,
        onAdmitted() {
          admissions++;
          options?.onAdmitted?.();
        }
      },
      ...args
    );

  const frameClosures = [];
  const dispatch = runtime.spotNodeRuntime.dispatchEntryActorPacket.bind(runtime.spotNodeRuntime);
  runtime.spotNodeRuntime.dispatchEntryActorPacket = async (...args) => {
    if (args[0] === 'local' && args[1] === actorId) {
      const closes = args[2].map((frame) => {
        const close = frame.close.bind(frame);
        const observed = { count: 0 };
        frame.close = () => {
          observed.count++;
          close();
        };
        return observed;
      });
      frameClosures.push(closes);
      if (beforeLocalAdmission) await beforeLocalAdmission();
    }
    return dispatch(...args);
  };

  async function relay(value) {
    const result = deferred();
    relayResults.push(result);
    await client.actor(actorId).send({ value }).packetName(packetName).submit();
    return result.promise;
  }

  await telemetry.observed();
  const first = relay('throw');
  await started.promise;
  assert.deepEqual(await first, { status: 'Submitted' });
  assert.deepEqual(await relay('second'), { status: 'Submitted' });
  assert.deepEqual(received, ['throw']);
  assert.equal(admissions, 2);
  assert.deepEqual(
    frameClosures.map((frames) => frames.map(({ count }) => count)),
    [
      [0, 0],
      [0, 0]
    ]
  );
  release.resolve();
  await scheduler.whenIdle();
  await nextTurn();
  await telemetry.observed();

  await t.test('second SEND executes once after the first releases the Actor turn', () => {
    assert.deepEqual(received, ['throw', 'second']);
    assert.deepEqual(
      frameClosures.map((frames) => frames.map(({ count }) => count)),
      [
        [1, 1],
        [1, 1]
      ]
    );
  });

  await t.test('accepted handler failure produces one dispatch error', () => {
    const failures = telemetry.records.filter(
      (event) => event.packetName === packetName && event.eventId === 'zlink.dispatch_error'
    );
    assert.equal(failures.length, 1);
    assert.equal(failures[0].reason, 'handler_exception');
    assert.equal(failures[0].errorMessage, 'session Actor SEND handler failed');
    assert.deepEqual(runtimeErrors, []);
  });

  async function assertRefusal(pending, kind) {
    const result = await pending;
    assert.equal(result.status, 'Rejected');
    assert.equal(result.error.kind, kind);
    assert.ok(result.error instanceof ZLinkFrameworkException);
  }

  await t.test(
    'SEND in flight when the Actor FIFO closes is rejected before admission',
    async (t) => {
      const submitting = deferred();
      const resume = deferred();
      t.after(() => {
        beforeLocalAdmission = undefined;
        resume.resolve();
      });
      const attempts = t.mock.method(scheduler, 'admit');
      beforeLocalAdmission = () => {
        submitting.resolve();
        return resume.promise;
      };
      const pending = relay('refused');
      await submitting.promise;
      assert.equal(scheduler.admissionClosed, false);
      scheduler.closeAdmission();
      resume.resolve();
      const result = await pending;
      await nextTurn();
      assert.equal(attempts.mock.callCount(), 1);
      assert.equal(admissions, 2);
      assert.deepEqual(received, ['throw', 'second']);
      assert.deepEqual(
        frameClosures[2].map(({ count }) => count),
        [1, 1]
      );
      await assertRefusal(result, ZLinkFrameworkErrorKind.Rejected);
    }
  );

  await t.test(
    'a binding with live authority but no local Actor route is unavailable',
    async (t) => {
      assert.ok(await actors.find(actorId));
      t.mock.method(actors, 'getState', () => undefined);
      await sessionStream.unbindActor(actorId, 2000);
      assert.equal(sessionStream.actorBinding(actorId), undefined);
      await assertRefusal(relay('refused'), ZLinkFrameworkErrorKind.Unavailable);
      assert.equal(frameClosures.length, 3);
    }
  );

  await t.test('closed Entry executor refuses Actor SEND admission', async () => {
    await entry.spotSerialExecutor.closeChildren();
    await assertRefusal(relay('refused'), ZLinkFrameworkErrorKind.Rejected);
  });

  await t.test('runtime shutdown refuses new relay admission', async (t) => {
    const submitting = deferred();
    const resume = deferred();
    const shutdownStarted = deferred();
    t.after(() => {
      beforeRelay = undefined;
      resume.resolve();
    });
    const close = runtime.admission.close.bind(runtime.admission);
    t.mock.method(runtime.admission, 'close', () => {
      close();
      shutdownStarted.resolve();
    });
    beforeRelay = () => {
      submitting.resolve();
      return resume.promise;
    };
    const pending = relay('refused');
    await submitting.promise;
    const shutdown = runtime.shutdown();
    try {
      await shutdownStarted.promise;
      resume.resolve();
      await assertRefusal(pending, ZLinkFrameworkErrorKind.ShuttingDown);
    } finally {
      await shutdown;
    }
  });
});
