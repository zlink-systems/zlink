'use strict';
require('reflect-metadata');
const test = require('node:test');
const assert = require('node:assert/strict');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const deferred = () => {
  let resolve;
  const promise = new Promise((r) => {
    resolve = r;
  });
  return { promise, resolve };
};
for (const phase of ['join', 'joined', 'leave']) {
  for (const awaitClose of [false, true]) {
    test(`Spot ${phase} rejects self close at invocation (await=${awaitClose})`, async () => {
      const rooms = new Map();
      let completion;
      let checks = 0;
      const checked = deferred();
      class Join {
        constructor(spotId) {
          this.spotId = spotId;
        }
      }
      class Player {
        constructor(context) {
          this.context = context;
        }
        async onJoinCompleted(result) {
          completion.resolve(result);
        }
      }
      class PlayerFactory {
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
      class Room {
        constructor(context) {
          this.context = context;
        }
        configure() {
          rooms.set(String(this.context.spotId), this);
        }
        async onInitialize() {
          assert.throws(() => this.context.close(), {
            kind: ZLinkFrameworkErrorKind.InvalidOperation
          });
        }
        async onActorJoin() {
          await this.checkClose('join');
          return { accepted: true };
        }
        async onJoinedActor() {
          await this.checkClose('joined');
        }
        async onLeaveActor() {
          await this.checkClose('leave');
        }
        async checkClose(currentPhase) {
          if (this.phase !== currentPhase) return;
          assert.throws(() => this.context.close(), {
            kind: ZLinkFrameworkErrorKind.InvalidOperation
          });
          if (awaitClose)
            await assert.rejects(
              (async () => {
                await this.context.close();
              })(),
              { kind: ZLinkFrameworkErrorKind.InvalidOperation }
            );
          assert.equal(await this.other.close(), true);
          checks++;
          checked.resolve();
        }
      }
      class JoinHandler {
        async handle(_spot, actor, _context, request) {
          actor.context.joinSpot(request.spotId).defer();
        }
      }
      nestjs.zlinkEntrySpotActorSendHandler({
        entrySpot: () => Entry,
        actor: () => Player,
        packetName: 'Join'
      })(JoinHandler);
      class MoveHandler extends JoinHandler {}
      nestjs.zlinkSpotActorSendHandler({
        spot: () => Room,
        actor: () => Player,
        packetName: 'Join'
      })(MoveHandler);
      const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
      builder
        .addRouteMesh('play')
        .listen(`inproc://close4432-${phase}-${awaitClose}`)
        .objects()
        .server()
        .addEntrySpot(Entry, 'Player')
        .addActorFactory('Player', PlayerFactory, (f) => f.disableRelocation())
        .addSpotFactory('Room', Room, (f) => f.disableRelocation());
      class TestModule {}
      Module({
        imports: [nestjs.ZLinkModule.forRoot(builder.build())],
        providers: [JoinHandler, MoveHandler]
      })(TestModule);
      const app = await NestFactory.createApplicationContext(TestModule, {
        logger: false,
        abortOnError: false
      });
      try {
        await app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).setMessageFlowModeAsync('detailed');
        const spots = app.get(nestjs.ZLINK_SPOT_MANAGER);
        const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
        const client = app.get(nestjs.ZLINK_ACTOR_CLIENT);
        const source = await spots.create('Room').inMesh('play').submit();
        const target = await spots.create('Room').inMesh('play').submit();
        const other = await spots.create('Room').inMesh('play').submit();
        const room = rooms.get(String(source.spot.spotId));
        room.phase = phase;
        room.other = rooms.get(String(other.spot.spotId)).context;
        await actors.create('player', 'Player').inMesh('play').submit();
        for (const spot of [source, target]) {
          completion = deferred();
          await client.sendToActor('player', new Join(String(spot.spot.spotId))).submit();
          const result = await completion.promise;
          assert.equal(result.status, 'accepted', require('node:util').inspect(result));
        }
        await checked.promise;
        assert.equal(checks, 1);
        assert.equal(await spots.close(source.spot), true);
      } finally {
        await app.close();
      }
    });
  }
}
