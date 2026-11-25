'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: nextTurn } = require('node:timers/promises');
const { randomUUID } = require('node:crypto');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');
process.env.ZLINK_TEST_MESSAGE_FLOW_STDERR = '1';
const telemetry = require('./helpers/telemetry-log-capture');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nestjs = require('../../packages/nestjs/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');

test(
  'typed Actor send failures and User Spot handler failures are recorded at their packet boundaries',
  { timeout: 30000 },
  async (t) => {
    const hostClock = fixedRidHostClock(t);
    const mesh = `failure-flow-${randomUUID()}`;
    const actorDelivered = deferred();
    const spotDelivered = deferred();

    class Snapshot {
      constructor(winner) {
        this.winner = winner;
      }
    }
    framework.ZLinkPacket('Snapshot', {
      payload: {
        type: 'object',
        properties: { winner: { type: 'string' } },
        required: ['winner']
      }
    })(Snapshot);

    class Player {
      constructor(context) {
        this.context = context;
        this.actorId = context.actorId;
      }
    }
    class PlayerFactory {
      async create(context) {
        return new Player(context);
      }
    }
    class Entry {
      async onCreateActor() {
        return { accepted: true };
      }
    }
    class SnapshotHandler {
      handle(_spot, _actor, _context, message) {
        actorDelivered.resolve(message.winner);
      }
    }
    class Room {}
    class CommitRoom {
      constructor(fail) {
        this.fail = fail;
      }
    }
    class CommitRoomHandler {
      async handle(_spot, message) {
        await nextTurn();
        if (message.fail) throw new Error('room commit failed');
        spotDelivered.resolve();
      }
    }
    nestjs.zlinkEntrySpotActorSendHandler({
      actor: Player,
      entrySpot: Entry,
      packetName: 'Snapshot'
    })(SnapshotHandler);
    nestjs.zlinkSpotPacketHandler({ spot: Room, packetName: 'CommitRoom' })(CommitRoomHandler);

    const builder = nestjs
      .zlinkFramework()
      .addLocationStore(new ZLinkInMemoryProviderLocationStore());
    builder.configureLocations().ownerLeaseFencingMarginMs(1);
    builder.configureDispatch().messageFlow('normal');
    builder
      .addRouteMesh(mesh)
      .listen('tcp://127.0.0.1:0')
      .objects()
      .server()
      .addEntrySpot(Entry, 'player')
      .addActorFactory('player', PlayerFactory, (factory) => factory.disableRelocation())
      .addSpotFactory('room', Room, (factory) => factory.disableRelocation());

    class App {}
    Module({
      imports: [nestjs.ZLinkModule.forRoot(builder.build())],
      providers: [Entry, PlayerFactory, SnapshotHandler, Room, CommitRoomHandler]
    })(App);
    t.after(() => telemetry.close());
    const app = await NestFactory.createApplicationContext(App, {
      logger: false,
      abortOnError: false
    });
    try {
      await finishFixedRidHostStartup(
        app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME),
        hostClock,
        framework.ZLinkFrameworkRuntimeState.Preparing
      );
      await app
        .get(nestjs.ZLINK_ACTOR_MANAGER)
        .getOrCreate('alice', 'player')
        .inMesh(mesh)
        .submit();
      await app.get(nestjs.ZLINK_SPOT_MANAGER).getOrCreate('room-1', 'room').inMesh(mesh).submit();
      const actor = app.get(nestjs.ZLINK_ACTOR_CLIENT);
      const spot = app.get(nestjs.ZLINK_SPOT_OUTBOUND);
      const records = (packetName) =>
        telemetry.records.filter((record) => record.packetName === packetName);
      const failures = (packetName) =>
        records(packetName).filter((record) => record.eventId === 'zlink.dispatch_error');

      await telemetry.observed();
      await t.test('Actor encode refusal records one failure and no sent event', async () => {
        await assert.rejects(
          actor.sendToActor('alice', new Snapshot(null)).submit(),
          /Snapshot.*\$\.winner.*nullable/
        );
        await telemetry.observed();
        assert.equal(records('Snapshot').filter((record) => record.phase === 'sent').length, 0);
        assert.equal(failures('Snapshot').length, 1, 'Actor encode error count');
        const [error] = failures('Snapshot');
        assert.equal(error.outcome, 'failed');
        assert.equal(error.reason, 'encode_error');
        assert.equal(error.action, 'fail_caller');
        assert.equal(error.actorId, 'alice');
        assert.match(error.errorMessage, /Snapshot.*\$\.winner.*nullable/);
        assertStderrFailure('Snapshot', 'encode_error', 'fail_caller', /winner.*nullable/);

        await actor.sendToActor('alice', new Snapshot('alice')).submit();
        assert.equal(await actorDelivered.promise, 'alice');
        await telemetry.observed();
        assert.equal(
          failures('Snapshot').length,
          1,
          'valid Actor send records no additional failure'
        );
      });

      await t.test('async User Spot one-way failure retains the received flow', async () => {
        await spot.sendToSpot('room-1', new CommitRoom(true)).submit();
        await spot.sendToSpot('room-1', new CommitRoom(false)).submit();
        await spotDelivered.promise;
        await telemetry.observed();

        const received = records('CommitRoom').filter((record) => record.phase === 'received');
        assert.equal(received.length, 2, 'User Spot received count');
        assert.equal(failures('CommitRoom').length, 1, 'User Spot handler error count');
        const [error] = failures('CommitRoom');
        assert.equal(error.outcome, 'failed');
        assert.equal(error.reason, 'handler_exception');
        assert.equal(error.action, 'drop');
        assert.equal(error.errorMessage, 'room commit failed');
        assertStderrFailure('CommitRoom', 'handler_exception', 'drop', /room commit failed/);
        assert.ok(received[0].flowId);
        assert.equal(error.flowId, received[0].flowId);
        assert.equal(error.flowOrigin, received[0].flowOrigin);
        assert.notEqual(error.flowId, received[1].flowId);
      });
    } finally {
      await app.close();
    }
  }
);

function assertStderrFailure(packet, reason, action, detail) {
  const failures = telemetry.stderrRecords
    .filter((line) => line.startsWith('zlink.dispatch_error '))
    .map((line) => JSON.parse(line.slice('zlink.dispatch_error '.length)))
    .filter((attributes) => attributes.packet_name === packet);
  assert.equal(failures.length, 1);
  const [error] = failures;
  assert.equal(error.outcome, 'failed');
  assert.equal(error.reason, reason);
  assert.equal(error.action, action);
  assert.match(error.error_message, detail);
}

function deferred() {
  let resolve;
  const promise = new Promise((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}
