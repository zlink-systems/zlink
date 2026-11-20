if (process.env.ZLINK_BOUND_CLOSE_FLOW_FILE) {
  const fs = require('node:fs');
  const { logs } = require('@opentelemetry/api-logs');
  const { LoggerProvider, SimpleLogRecordProcessor } = require('@opentelemetry/sdk-logs');
  logs.setGlobalLoggerProvider(
    new LoggerProvider({
      processors: [
        new SimpleLogRecordProcessor({
          exporter: {
            export(records, done) {
              for (const record of records)
                fs.appendFileSync(
                  process.env.ZLINK_BOUND_CLOSE_FLOW_FILE,
                  JSON.stringify({ body: record.body, ...record.attributes }) + '\n'
                );
              done({ code: 0 });
            },
            async shutdown() {},
            async forceFlush() {}
          }
        })
      ]
    })
  );
}
const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const framework = require('../../packages/framework/dist/internal');
const connector = require('../../packages/stream-connector/dist');
const { createBrowserSafeStreamApp } = require('./helpers/browser-safe-stream-app');

const {
  ZLinkSpotNodeRuntimeManager
} = require('../../packages/framework/dist/runtime/spots/spot-node-runtime-manager');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');
const {
  ZLinkManagedStream
} = require('../../packages/framework/dist/runtime/streams/managed-stream');
const {
  decodeStreamFrame,
  decodeStreamHeader
} = require('../../packages/framework/dist/runtime/streams/protocol');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

for (const scenario of ['reply', 'push']) {
  test(
    `${scenario} precedes ${scenario === 'reply' ? 'next-turn close' : 'close'} on the same node (40 repetitions)`,
    { timeout: 120000 },
    async (t) => {
      const apps = [];
      const clients = [];
      let actorRef;
      let returned;
      let release;
      let closed;
      let closeEntered;
      let lookupEntered;
      const dispatch = ZLinkSpotNodeRuntimeManager.prototype.dispatchEntryActorPacket;
      const close = ServiceStatefulRuntime.prototype.closeBoundSession;
      const submit = ZLinkManagedStream.prototype.submitRaw;
      const lookup = ZLinkActorSessionBindingRegistry.prototype.routeForSessionBinding;
      ZLinkActorSessionBindingRegistry.prototype.routeForSessionBinding = async function (...args) {
        if (scenario === 'push' && lookupEntered) {
          const entered = lookupEntered;
          lookupEntered = undefined;
          entered.resolve();
          await release.promise;
        }
        return lookup.apply(this, args);
      };
      ZLinkSpotNodeRuntimeManager.prototype.dispatchEntryActorPacket = async function (...args) {
        const result = await dispatch.apply(this, args);
        if (scenario === 'reply' && args[0] === 'local' && args[3]) {
          returned.resolve();
          await release.promise;
        }
        return result;
      };
      ServiceStatefulRuntime.prototype.closeBoundSession = function (...args) {
        const result = close.apply(this, args);
        closeEntered.resolve();
        return result;
      };
      ZLinkManagedStream.prototype.submitRaw = async function (...args) {
        if (
          scenario === 'push' &&
          lookupEntered &&
          decodeStreamHeader(decodeStreamFrame(args[0].data()).header).name === 'Notice'
        ) {
          const entered = lookupEntered;
          lookupEntered = undefined;
          entered.resolve();
          await release.promise;
        }
        return submit.apply(this, args);
      };
      t.after(async () => {
        release?.resolve();
        ZLinkSpotNodeRuntimeManager.prototype.dispatchEntryActorPacket = dispatch;
        ServiceStatefulRuntime.prototype.closeBoundSession = close;
        ZLinkManagedStream.prototype.submitRaw = submit;
        ZLinkActorSessionBindingRegistry.prototype.routeForSessionBinding = lookup;
        for (const client of clients) await client.close();
        for (const app of apps.reverse()) await app.close();
      });
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
        async onDisconnectActor() {
          closed.resolve();
        }
      }
      class Reply {
        async handle() {
          return { value: 'reply' };
        }
      }
      class Push {
        async handle(_spot, actor) {
          const sending = actor.context.boundSession
            .send({ value: 'push' })
            .packetName('Notice')
            .submit();
          await actor.context.boundSession.disconnect();
          await sending;
          return {};
        }
      }
      class Drop {
        async handle(_spot, actor) {
          await actor.context.boundSession.disconnect();
          return {};
        }
      }
      for (const [handler, packetName] of [
        [Reply, 'Reply'],
        [Push, 'Push'],
        [Drop, 'Drop']
      ]) {
        nestjs.zlinkEntrySpotActorRequestHandler({
          entrySpot: () => Entry,
          actor: () => Player,
          packetName
        })(handler);
      }
      class SessionFactory {
        async create(context) {
          return {
            context,
            async onDispatch(dispatch, message) {
              if (dispatch.actor) {
                await dispatch.actor.relay(dispatch, message);
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
        .addRouteMesh('order')
        .listen('tcp://127.0.0.1:0')
        .objects()
        .server()
        .addEntrySpot(Entry, 'player')
        .addActorFactory('player', Factory, (f) => f.disableRelocation());
      builder
        .addStreamNode('clients')
        .bind('ws://127.0.0.1:*')
        .enableActorDispatch()
        .registerSession(SessionFactory);
      class AppModule {}
      Module({
        imports: [nestjs.ZLinkModule.forRoot(builder.build())],
        providers: [Factory, SessionFactory, Reply, Push, Drop]
      })(AppModule);
      const app = await createBrowserSafeStreamApp(
        () =>
          NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false }),
        'clients'
      );
      apps.push(app);
      const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
      const actorClient = app.get(nestjs.ZLINK_ACTOR_CLIENT);
      await actors.getOrCreate('ordered-player', 'player').inMesh('order').submit();
      actorRef = await actors.find('ordered-player');
      const failures = [];
      let receivedBeforeClose = 0;
      for (let iteration = 0; iteration < 40; iteration++) {
        returned = deferred();
        release = deferred();
        closed = deferred();
        closeEntered = deferred();
        const events = [];
        const eof = deferred();
        const client = connector.zlinkStreamConnectorFactory.create({
          endpoint: app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).getListenerStatus('stream', 'clients')
            .endpoint,
          reconnect: { enabled: false },
          dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
        });
        clients.push(client);
        client.onDisconnected(() => {
          events.push('EOF');
          eof.resolve();
        });
        await client.connect();
        assert.deepEqual(await client.request({}).packetName('Bind').submit(), { bound: true });
        let response;
        let pushing;
        if (scenario === 'reply') {
          response = client
            .actor('ordered-player')
            .request({})
            .packetName('Reply')
            .submit()
            .then(
              (value) => {
                events.push('reply');
                return value;
              },
              (error) => error
            );
          await returned.promise;
        } else {
          client.actor('ordered-player').on('Notice', () => {
            events.push('push');
          });
          lookupEntered = deferred();
          const entered = lookupEntered;
          pushing = actorClient
            .requestToActor('ordered-player', {})
            .packetName('Push')
            .submit()
            .catch((error) => error);
          await entered.promise;
        }
        const dropping =
          scenario === 'reply'
            ? actorClient.requestToActor('ordered-player', {}).packetName('Drop').submit()
            : Promise.resolve();
        await closeEntered.promise;
        await new Promise((resolve) => setImmediate(resolve));
        release.resolve();
        await dropping;
        const pushResult = await pushing;
        if (pushResult instanceof Error)
          failures.push(`iteration ${iteration}: ${pushResult.stack}`);
        await eof.promise;
        await closed.promise;
        if (response) await response;
        await client.close();
        try {
          assert.deepEqual(events, [scenario, 'EOF']);
          if (!(pushResult instanceof Error)) receivedBeforeClose++;
        } catch (error) {
          failures.push(`iteration ${iteration}: ${error.message}`);
        }
      }
      t.diagnostic(`${scenario}: ${receivedBeforeClose}/40 client deliveries before EOF`);
      assert.deepEqual(failures, []);
    }
  );
}

const { sessionBindingControlPorts } = require('./fixtures/session-binding-control');
const { withTestSessionOwners } = require('./fixtures/session-owner-descriptors');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

test('queued close retains its original deadline and cannot close after expiry', async () => {
  const runtime = withTestSessionOwners(
    new ServiceStatefulRuntime(
      {
        ...sessionBindingControlPorts(),
        setServiceIngress() {}
      },
      'owner',
      1n
    )
  );
  const actor = runtime.createActor('deadline-player').ref;
  let closes = 0;
  await runtime.bindSession(
    'session',
    actor,
    1000,
    async () => true,
    undefined,
    undefined,
    async () => {
      closes++;
    }
  ).promise;
  const binding = runtime.sessionBindings('session')[0];
  const release = deferred();
  const submission = runtime.captureSessionSubmission(binding)(() => release.promise);
  const closing = runtime.closeBoundSession(actor, binding.bindingGeneration, 1000);
  const observed = closing.promise.catch((error) => error);
  await new Promise((resolve) => setImmediate(resolve));
  runtime.expireOperations(Infinity, Infinity);
  const terminal = await observed;
  assert.notEqual(terminal.terminalResult, RequestResult.Ok);
  release.resolve();
  await submission;
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(closes, 0);
  runtime.close();
});
