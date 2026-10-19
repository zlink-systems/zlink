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

for (const remote of [false, true])
  for (const join of [false, true]) {
    test(
      `Actor handler disconnect closes client connection (remote=${remote}, join=${join})`,
      { timeout: 20000 },
      async (t) => {
        const provider = new framework.ZLinkInMemoryProviderLocationStore();
        const apps = [];
        t.after(async () => {
          for (const app of apps.reverse()) await app.close();
        });
        let actorRef;
        let finishJoin;
        const joined = new Promise((resolve) => {
          finishJoin = resolve;
        });
        let finishDisconnect;
        const notified = new Promise((resolve) => {
          finishDisconnect = resolve;
        });
        let disconnectCount = 0;
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
            disconnectCount++;
            finishDisconnect();
          }
        }
        class Room {
          constructor(context) {
            this.context = context;
          }
          configure() {}
          async onActorJoin() {
            return { accepted: true };
          }
          async onJoinedActor() {
            finishJoin();
          }
          async onLeaveActor() {}
          async onDisconnectActor() {
            disconnectCount++;
            finishDisconnect();
          }
        }
        class DropEntry {
          async handle(_spot, actor) {
            await actor.context.boundSession.disconnect();
            return { closed: true };
          }
        }
        class DropRoom {
          async handle(_spot, actor) {
            await actor.context.boundSession.disconnect();
            return { closed: true };
          }
        }
        class Move {
          async handle(_spot, actor) {
            actor.context.joinSpot('close-room').defer();
            return {};
          }
        }
        nestjs.zlinkEntrySpotActorRequestHandler({
          entrySpot: () => Entry,
          actor: () => Player,
          packetName: 'Drop'
        })(DropEntry);
        nestjs.zlinkEntrySpotActorRequestHandler({
          entrySpot: () => Entry,
          actor: () => Player,
          packetName: 'Move'
        })(Move);
        nestjs.zlinkSpotActorRequestHandler({
          spot: () => Room,
          actor: () => Player,
          packetName: 'Drop'
        })(DropRoom);
        class SessionFactory {
          async create(context) {
            return {
              context,
              async onDispatch() {
                await context.actors.bindOrGet(actorRef);
                await context.client.reply({ bound: true }).submit();
              }
            };
          }
        }
        async function start(sessionOnly) {
          const builder = nestjs.zlinkFramework().addLocationStore(provider);
          builder.configureLocations().pollingIntervalMs(20);
          builder.configureDispatch().messageFlow('normal');
          const mesh = builder.addRouteMesh('close').listen('tcp://127.0.0.1:0');
          if (sessionOnly) mesh.objects().client();
          else
            mesh
              .objects()
              .server()
              .addEntrySpot(Entry, 'player')
              .addActorFactory('player', Factory, (f) => f.disableRelocation())
              .addSpotFactory('room', Room, (f) => f.disableRelocation());
          if (sessionOnly || !remote)
            builder
              .addStreamNode('clients')
              .bind('ws://127.0.0.1:*')
              .enableActorDispatch()
              .registerSession(SessionFactory);
          class AppModule {}
          Module({
            imports: [nestjs.ZLinkModule.forRoot(builder.build())],
            providers: sessionOnly
              ? [SessionFactory]
              : [Factory, SessionFactory, DropEntry, DropRoom, Move]
          })(AppModule);
          const app = await NestFactory.createApplicationContext(AppModule, {
            logger: false,
            abortOnError: false
          });
          apps.push(app);
          return app;
        }
        const owner = await start(false);
        const actors = owner.get(nestjs.ZLINK_ACTOR_MANAGER);
        await actors.getOrCreate('close-player', 'player').inMesh('close').submit();
        actorRef = await actors.find('close-player');
        await actors.getOrCreate('unbound-player', 'player').inMesh('close').submit();
        await assert.rejects(
          () =>
            owner
              .get(nestjs.ZLINK_ACTOR_CLIENT)
              .requestToActor('unbound-player', {})
              .packetName('Drop')
              .submit(),
          (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
        );
        const sessionOwner = remote ? await start(true) : owner;
        if (remote) {
          const deadline = Date.now() + 5000;
          while (
            sessionOwner.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('close').readyPeerCount < 1
          ) {
            assert.ok(Date.now() < deadline, 'Session owner peer readiness');
            await new Promise((resolve) => setTimeout(resolve, 20));
          }
        }
        const client = connector.zlinkStreamConnectorFactory.create({
          endpoint: sessionOwner
            .get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
            .getListenerStatus('stream', 'clients').endpoint,
          dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
        });
        t.after(async () => await client.close());
        let finishPhysical;
        const physical = new Promise((resolve) => {
          finishPhysical = resolve;
        });
        client.onDisconnected(() => {
          finishPhysical();
        });
        await client.connect();
        assert.deepEqual(await client.request({}).packetName('Bind').submit(), { bound: true });
        const clientActors = owner.get(nestjs.ZLINK_ACTOR_CLIENT);
        if (join) {
          await owner
            .get(nestjs.ZLINK_SPOT_MANAGER)
            .getOrCreate('close-room', 'room')
            .inMesh('close')
            .submit();
          await clientActors.requestToActor('close-player', {}).packetName('Move').submit();
          await joined;
        }
        assert.deepEqual(
          await clientActors.requestToActor('close-player', {}).packetName('Drop').submit(),
          { closed: true }
        );
        await physical;
        await notified;
        assert.equal(disconnectCount, 1);
      }
    );
  }
