'use strict';
require('reflect-metadata');
const test = require('node:test');
const assert = require('node:assert/strict');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const {
  ZLinkExecutionBarrier,
  ZLinkSerialExecutionQueue,
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/internal');
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
      const closingCalls = new Map();
      let completion;
      let completionCalls = 0;
      let acceptedCalls = 0;
      let lateApplicationCalls = 0;
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
          completionCalls++;
          if (result.status === 'accepted') acceptedCalls++;
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
        async onClosing() {
          const spotId = String(this.context.spotId);
          closingCalls.set(spotId, (closingCalls.get(spotId) ?? 0) + 1);
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
      class LateApplication {}
      class LateApplicationHandler {
        handle() {
          lateApplicationCalls++;
        }
      }
      nestjs.zlinkSpotActorSendHandler({
        spot: () => Room,
        actor: () => Player,
        packetName: 'Join'
      })(MoveHandler);
      nestjs.zlinkSpotPacketHandler({
        spot: () => Room,
        packetName: 'LateApplication'
      })(LateApplicationHandler);
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
        providers: [JoinHandler, MoveHandler, LateApplicationHandler]
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
        const outbound = app.get(nestjs.ZLINK_SPOT_OUTBOUND);
        const source = await spots.create('Room').inMesh('play').submit();
        const target = await spots.create('Room').inMesh('play').submit();
        const other = await spots.create('Room').inMesh('play').submit();
        const room = rooms.get(String(source.spot.spotId));
        room.phase = phase;
        room.other = rooms.get(String(other.spot.spotId)).context;
        await actors.create('player', 'Player').inMesh('play').submit();
        for (const spot of [source, target]) {
          completion = deferred();
          completionCalls = 0;
          acceptedCalls = 0;
          await client.sendToActor('player', new Join(String(spot.spot.spotId))).submit();
          const result = await completion.promise;
          assert.equal(result.status, 'accepted', require('node:util').inspect(result));
          assert.equal(completionCalls, 1);
          assert.equal(acceptedCalls, 1);
        }
        await checked.promise;
        assert.equal(checks, 1);
        assert.equal(await spots.close(source.spot), true);
        assert.equal(closingCalls.get(String(source.spot.spotId)), 1);
        await assert.rejects(
          outbound
            .sendToSpot(String(source.spot.spotId), new LateApplication())
            .inMesh('play')
            .submit()
        );
        assert.equal(lateApplicationCalls, 0);
      } finally {
        await app.close();
      }
    });
  }
}

test('a resumed segment keeps its claim until the logical turn terminal', async () => {
  const barrier = new ZLinkExecutionBarrier();
  const serial = new ZLinkSpotSerialTurnExecutor();
  serial.setExecutionBarrier(barrier);

  const firstBoundary = deferred();
  const secondBoundary = deferred();
  const started = deferred();
  const secondBoundaryStarted = deferred();
  const firstContinuationTerminal = deferred();
  const logicalTerminal = deferred();
  let continuationCount = 0;
  let closeQuiesced = false;
  let seal;
  let submitted;
  const queuePrototype = ZLinkSerialExecutionQueue.prototype;
  const submitContinuation = queuePrototype.submitContinuation;
  try {
    queuePrototype.submitContinuation = function (operation, context) {
      const terminal = submitContinuation.call(this, operation, context);
      if (continuationCount++ === 0) {
        void terminal.then(
          () => firstContinuationTerminal.resolve(),
          () => firstContinuationTerminal.resolve()
        );
      }
      return terminal;
    };
    submitted = serial.execute(async () => {
      const turn = serial.currentTurn;
      assert.ok(turn);
      started.resolve();
      try {
        await turn.yieldFrameworkPromise(firstBoundary.promise);
        secondBoundaryStarted.resolve();
        await turn.yieldFrameworkPromise(secondBoundary.promise);
      } finally {
        logicalTerminal.resolve();
      }
    });

    await started.promise;
    seal = barrier.seal('close');
    const closeTerminal = barrier.waitForQuiescence(seal).then(() => {
      closeQuiesced = true;
    });

    firstBoundary.resolve();
    await firstContinuationTerminal.promise;
    await secondBoundaryStarted.promise;
    assert.equal(
      closeQuiesced,
      false,
      'a yielded logical turn still owns its claim while its next boundary is pending'
    );

    secondBoundary.resolve();
    await logicalTerminal.promise;
    await closeTerminal;
    assert.equal(closeQuiesced, true);
    await submitted;
  } finally {
    queuePrototype.submitContinuation = submitContinuation;
    secondBoundary.resolve();
    await Promise.allSettled([logicalTerminal.promise, submitted]);
    if (seal !== undefined) barrier.abort(seal);
  }
});
