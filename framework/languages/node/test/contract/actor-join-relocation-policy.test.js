const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const deferred = require('./helpers/deferred.cjs');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');

const ACTOR_JOIN_WIRE_COMMAND = 28;

if (process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR) require('./helpers/telemetry-log-capture');

for (const mixedFactories of [false, true]) {
  test(
    `source checks the remote Actor Join policy with ${mixedFactories ? 'mixed' : 'disabled'} factories`,
    { timeout: 15_000 },
    async (t) => {
      const clock = fixedRidHostClock(t);
      const locations = new ZLinkInMemoryProviderLocationStore();
      const relocations = new MemoryRelocationStore();
      const apps = [];
      t.after(async () => {
        for (const app of apps.reverse()) await app.close();
      });
      let wireRequests = 0;
      const requestService = RawServiceMeshRuntime.prototype.requestService;
      t.mock.method(
        RawServiceMeshRuntime.prototype,
        'requestService',
        function (rid, parts, ...args) {
          if (parts[0]?.[3] === ACTOR_JOIN_WIRE_COMMAND) wireRequests++;
          return requestService.call(this, rid, parts, ...args);
        }
      );

      let admissions = 0;
      const completions = new Map();
      class Join {
        constructor(spotId) {
          this.spotId = spotId;
        }
      }
      class Player {
        constructor(context) {
          this.context = context;
        }
        async onJoinCompleted(completion) {
          completions.get(this.context.actorId).resolve(completion);
        }
      }
      class Factory {
        async create(context) {
          return new Player(context);
        }
      }
      class Entry {}
      class Room {
        async onActorJoin(actorId) {
          admissions++;
          return { accepted: actorId !== 'enabled' };
        }
        async onJoinedActor() {}
        async onLeaveActor() {}
      }
      class JoinHandler {
        async handle(_entry, actor, _context, request) {
          actor.context.joinSpot(request.spotId, request).defer();
        }
      }
      nest.zlinkEntrySpotActorSendHandler({
        entrySpot: () => Entry,
        actor: () => Player,
        packetName: 'Join'
      })(JoinHandler);

      async function start(rid) {
        const builder = nest
          .zlinkFramework()
          .addLocationStore(locations)
          .addRelocationStore(relocations);
        builder.configureDispatch().messageFlow('normal');
        builder.configureLocations().ownerLeaseFencingMarginMs(1);
        const mesh = builder.addRouteMesh('play').routingId(rid).listen('tcp://127.0.0.1:0');
        mesh.channel('play').server();
        const objects = mesh.objects().server();
        objects.addEntrySpot(Entry, mixedFactories && rid === 'target' ? 'enabled' : 'disabled');
        objects.addActorFactory('disabled', Factory, (factory) => factory.disableRelocation());
        if (mixedFactories) {
          objects.addActorFactory('enabled', Factory, (factory) => factory.recreateOnRelocation());
        }
        objects.addSpotFactory(`${rid}-room`, Room, (factory) => factory.disableRelocation());
        class Host {}
        Module({
          imports: [nest.ZLinkModule.forRoot(builder.build())],
          providers: [JoinHandler]
        })(Host);
        const app = await NestFactory.createApplicationContext(Host, {
          logger: false,
          abortOnError: false
        });
        apps.push(app);
        await finishFixedRidHostStartup(
          app.get(nest.ZLINK_FRAMEWORK_RUNTIME),
          clock,
          framework.ZLinkFrameworkRuntimeState.Preparing
        );
        return app;
      }

      async function create(app, rid, actorId, type) {
        const result = await app
          .get(nest.ZLINK_ACTOR_MANAGER)
          .create(actorId, type)
          .inMesh('play')
          .submit();
        assert.equal(String(result.actor.nodeRid), rid);
      }

      async function join(app, actorId, spotId, expected) {
        const completion = deferred();
        completions.set(actorId, completion);
        await app.get(nest.ZLINK_ACTOR_CLIENT).sendToActor(actorId, new Join(spotId)).submit();
        const { status, kind } = await completion.promise;
        assert.deepEqual({ status, kind, admissions, wireRequests }, expected);
      }

      async function createRoom(app, rid) {
        const room = await app
          .get(nest.ZLINK_SPOT_MANAGER)
          .getOrCreate(`${rid}-room`, `${rid}-room`)
          .inMesh('play')
          .submit();
        assert.equal(String(room.spot.nodeRid), rid);
      }

      const source = await start('source');
      await createRoom(source, 'source');
      await create(source, 'source', 'local', 'disabled');
      await create(source, 'source', 'remote', 'disabled');
      const target = await start('target');
      for (const app of [source, target]) {
        for await (const observed of app
          .get(nest.ZLINK_ROUTE_MESH_RUNTIME)
          .observe('play', 64, t.signal)) {
          if (observed.status.isReady && observed.status.readyPeerCount >= 1) break;
        }
      }
      await createRoom(target, 'target');
      if (mixedFactories) await create(target, 'target', 'enabled', 'enabled');
      await join(source, 'local', 'source-room', {
        status: 'accepted',
        kind: undefined,
        admissions: 1,
        wireRequests: 0
      });
      admissions = 0;
      await join(source, 'remote', 'target-room', {
        status: 'failed',
        kind: framework.ZLinkFrameworkErrorKind.Rejected,
        admissions: 0,
        wireRequests: 0
      });
      if (mixedFactories) {
        await join(target, 'enabled', 'source-room', {
          status: 'rejected',
          kind: undefined,
          admissions: 1,
          wireRequests: 1
        });
      }
    }
  );
}
