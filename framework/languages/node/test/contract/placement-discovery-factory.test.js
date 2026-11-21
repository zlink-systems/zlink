'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: boundary } = require('node:timers/promises');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');

// Discovery controls the projection; the real Store reservation and owner factories still run.
test(
  'Preparing discovery resumes exactly one Actor and User Spot factory; Ready lookup bypasses placement (40 rounds)',
  { timeout: 60000 },
  async (t) => {
    let actorsCreated = 0;
    let spotsCreated = 0;
    class Entry {
      constructor(context) {
        this.context = context;
      }
      async onCreateActor() {
        return { accepted: true };
      }
    }
    class ActorFactory {
      async create(context) {
        actorsCreated++;
        return { actorId: context.actorId, context };
      }
    }
    class Room {
      configure() {}
      async onInitialize() {
        spotsCreated++;
      }
    }
    const builder = nestjs
      .zlinkFramework()
      .addLocationStore(new framework.ZLinkInMemoryProviderLocationStore());
    builder
      .addRouteMesh('play')
      .listen('tcp://127.0.0.1:0')
      .objects()
      .server()
      .addEntrySpot(Entry, 'actor')
      .addActorFactory('actor', ActorFactory, (f) => f.disableRelocation())
      .addSpotFactory('room', Room, (f) => f.disableRelocation());
    class App {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())] })(App);
    const app = await NestFactory.createApplicationContext(App, {
      logger: false,
      abortOnError: false
    });
    try {
      const host = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
      const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER, { strict: false });
      const spots = app.get(nestjs.ZLINK_SPOT_MANAGER, { strict: false });
      // Host startup must have published the local row into the same discovery owner.
      await actors.create('startup-actor', 'actor').inMesh('play').timeout(1000).submit();
      await spots.getOrCreate('startup-spot', 'room').inMesh('play').timeout(1000).submit();
      assert.equal(actorsCreated, 1);
      assert.equal(spotsCreated, 1);
      const runtime = host.locationOwner.currentRuntime;
      const discovery = host.spotNodeRuntime.locationAutoConnect;
      const list = runtime.listLiveMeshNodes.bind(runtime);
      let projection = 'preparing';
      t.mock.method(runtime, 'listLiveMeshNodes', async (...args) =>
        (await list(...args)).map((row) => ({
          ...row,
          state: projection === 'serving' ? 1 : 0,
          ...(projection === 'full'
            ? {
                placementWeight: 0,
                populationCapacity: {
                  ...row.populationCapacity,
                  actors: { active: 1, reserved: 0, limit: 1 },
                  spots: { active: 1, reserved: 0, limit: 1 }
                }
              }
            : {})
        }))
      );
      const refresh = () => discovery.resolver.listLivePeers({ meshName: 'play' });
      const store = host.locationOwner.locationStore();
      const reserve = t.mock.method(store, 'reserve');
      for (let round = 1; round <= 40; round++) {
        await t.test(`round ${round}`, async () => {
          projection = 'preparing';
          await refresh();
          const calls = reserve.mock.calls.length;
          let settled = 0;
          const actor = actors
            .create(`actor-${round}`, 'actor')
            .inMesh('play')
            .timeout(1000)
            .submit();
          const spot = spots
            .getOrCreate(`spot-${round}`, 'room')
            .inMesh('play')
            .timeout(1000)
            .submit();
          const pending = Promise.all([actor, spot]).finally(() => {
            settled++;
          });
          pending.catch(() => undefined);
          await boundary();
          assert.equal(settled, 0, `round ${round}: pending before discovery refresh`);
          assert.equal(reserve.mock.calls.length, calls);
          projection = 'serving';
          await refresh();
          const [createdActor, createdSpot] = await pending;
          assert.equal(createdActor.status, 'created');
          assert.equal(actorsCreated, round + 1);
          assert.equal(spotsCreated, round + 1);
          assert.equal(reserve.mock.calls.length, calls + 2);
          projection = 'full';
          await refresh();
          const existingActor = await actors
            .getOrCreate(`actor-${round}`, 'actor')
            .inMesh('play')
            .timeout(1000)
            .submit();
          const existingSpot = await spots
            .getOrCreate(`spot-${round}`, 'room')
            .inMesh('play')
            .timeout(1000)
            .submit();
          assert.equal(existingActor.status, 'existing');
          assert.deepEqual(existingActor.actor, createdActor.actor);
          assert.deepEqual(existingSpot.spot, createdSpot.spot);
          assert.equal(reserve.mock.calls.length, calls + 2);
          assert.equal(actorsCreated, round + 1);
          assert.equal(spotsCreated, round + 1);
        });
      }
    } finally {
      await app.close();
    }
  }
);
