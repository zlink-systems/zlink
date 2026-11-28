'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: nextTurn } = require('node:timers/promises');
const telemetry = require('./helpers/telemetry-log-capture');
const deferred = require('./helpers/deferred.cjs');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
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
              await dispatch.actor.relay(dispatch, message);
              terminal.resolve({ status: 'Submitted' });
            } catch (error) {
              terminal.resolve({ status: 'Rejected', error });
            }
            return;
          }
          await context.actors.bindOrGet(actorRef);
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
  runtime.spotNodeRuntime.dispatchEntryActorPacket = (...args) => {
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

  await t.test('closed FIFO rejects relay with the current binding still active', async () => {
    scheduler.closeAdmission();
    const refusal = await relay('refused');
    await nextTurn();
    assert.equal(admissions, 2);
    assert.deepEqual(received, ['throw', 'second']);
    assert.deepEqual(
      frameClosures[2].map(({ count }) => count),
      [1, 1]
    );
    assert.equal(refusal.status, 'Rejected', 'caller result from the closed FIFO');
    assert.match(refusal.error.message, /serial execution queue is closed/);
  });
});
