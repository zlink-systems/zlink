'use strict';
require('reflect-metadata');
const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const telemetry = require('./helpers/telemetry-log-capture');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

class Move {
  constructor(target) {
    this.target = target;
  }
}
class Ping {
  constructor(token, sequence = 0) {
    this.token = token;
    this.sequence = sequence;
  }
}
class Notice {
  constructor(sequence) {
    this.sequence = sequence;
  }
}
class Missing {}

async function fixture() {
  const calls = [];
  const joins = [];
  const notices = new Map();
  let completion;
  let handlerPause;
  let joinedPause;
  let pingPause;
  class Player {
    constructor(context) {
      this.context = context;
    }
    async onJoinCompleted(result) {
      joins.push(result);
      completion?.resolve(result);
    }
  }
  class Factory {
    async create(context) {
      return new Player(context);
    }
  }
  class MoveHandler {
    async handle(_spot, actor, _context, message) {
      if (message.target === undefined) actor.context.joinEntrySpot().defer();
      else actor.context.joinSpot(message.target, {}).defer();
      if (handlerPause !== undefined) {
        handlerPause.entered.resolve();
        await handlerPause.release.promise;
      }
    }
  }
  class PingHandler {
    async handle(spot, actor, context, message) {
      calls.push({
        kind: 'ping',
        surface: spot.surface,
        token: message.token,
        sequence: message.sequence,
        correlation: context.correlationId
      });
      if (pingPause?.token === message.token) {
        pingPause.entered.resolve();
        await pingPause.release.promise;
      }
      return { token: message.token, surface: spot.surface, actorId: actor.context.actorId };
    }
  }
  class NoticeHandler {
    async handle(spot, _actor, _context, message) {
      calls.push({ kind: 'notice', surface: spot.surface, sequence: message.sequence });
      notices.get(message.sequence)?.resolve();
    }
  }
  function configure() {
    this.context.handlers.addHandler(MoveHandler).addHandler(PingHandler).addHandler(NoticeHandler);
  }
  async function joined() {
    if (joinedPause !== undefined && joinedPause.surface === this.surface) {
      joinedPause.entered.resolve();
      await joinedPause.release.promise;
    }
  }
  class Entry {
    surface = 'entry';
    configure = configure;
    onJoinedActor = joined;
    async onLeaveActor() {}
  }
  class Room {
    surface = 'user';
    configure = configure;
    onJoinedActor = joined;
    async onActorJoin() {
      return { accepted: true };
    }
    async onLeaveActor() {}
  }
  for (const Type of [Factory, Entry, MoveHandler, PingHandler, NoticeHandler]) Injectable()(Type);
  Injectable({ scope: Scope.TRANSIENT })(Room);
  framework.ZLinkSpotActorSend('Move')(
    MoveHandler.prototype,
    'handle',
    Object.getOwnPropertyDescriptor(MoveHandler.prototype, 'handle')
  );
  framework.ZLinkSpotActorRequest('Ping')(
    PingHandler.prototype,
    'handle',
    Object.getOwnPropertyDescriptor(PingHandler.prototype, 'handle')
  );
  framework.ZLinkSpotActorSend('Notice')(
    NoticeHandler.prototype,
    'handle',
    Object.getOwnPropertyDescriptor(NoticeHandler.prototype, 'handle')
  );
  const builder = nest.zlinkFramework();
  builder.configureDispatch().messageFlow('normal');
  builder
    .addRouteMesh('move-ingress')
    .listen('tcp://127.0.0.1:0')
    .setRoutingIdPrefix('move-ingress')
    .objects()
    .server()
    .addActorFactory('player', Factory, (factory) => factory.disableRelocation())
    .addEntrySpot(Entry, 'player')
    .addSpotFactory('room', Room, (factory) => factory.disableRelocation());
  class AppModule {}
  Module({
    imports: [
      nest.ZLinkModule.forRoot({ ...builder.build(), locations: { useInMemoryStores: true } })
    ],
    providers: [Factory, Entry, Room, MoveHandler, PingHandler, NoticeHandler]
  })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  try {
    const client = app.get(nest.ZLINK_ACTOR_CLIENT);
    const actors = app.get(nest.ZLINK_ACTOR_MANAGER);
    await app
      .get(nest.ZLINK_ACTOR_MANAGER)
      .getOrCreate('player', 'player')
      .inMesh('move-ingress')
      .submit();
    const roomId = (
      await app.get(nest.ZLINK_SPOT_MANAGER).create('room').inMesh('move-ingress').submit()
    ).spot.spotId;
    await telemetry.observed();
    return {
      app,
      client,
      actors,
      roomId,
      calls,
      joins,
      move(target, actorId = 'player') {
        completion = deferred();
        return {
          submitted: client.sendToActor(actorId, new Move(target)).submit(),
          completed: completion.promise
        };
      },
      pauseHandler() {
        handlerPause = { entered: deferred(), release: deferred() };
        return handlerPause;
      },
      pauseJoined(surface) {
        joinedPause = { surface, entered: deferred(), release: deferred() };
        return joinedPause;
      },
      pausePing(token) {
        pingPause = { token, entered: deferred(), release: deferred() };
        return pingPause;
      },
      ping(token, sequence, actorId = 'player') {
        return client.requestToActor(actorId, new Ping(token, sequence)).submit();
      },
      notice(sequence) {
        const handled = deferred();
        notices.set(sequence, handled);
        const submitted = client.sendToActor('player', new Notice(sequence)).submit();
        return Object.assign(Promise.all([submitted, handled.promise]), { submitted });
      }
    };
  } catch (error) {
    await app.close();
    throw error;
  }
}

async function completeMove(f, target) {
  const move = f.move(target);
  await move.submitted;
  assert.equal((await move.completed).status, 'accepted');
}

test(
  'same-tick User to Entry executes each original request once at the current membership (200 moves)',
  { timeout: 60000 },
  async () => {
    const f = await fixture();
    try {
      assert.deepEqual(await f.ping('stable-entry'), {
        token: 'stable-entry',
        surface: 'entry',
        actorId: 'player'
      });
      for (let iteration = 0; iteration < 200; iteration++) {
        await completeMove(f, f.roomId);
        assert.equal((await f.ping('stable-user')).surface, 'user');
        const token = `entry-${iteration}`;
        const move = f.move();
        const ping = f.ping(token);
        const [, result, joined] = await Promise.all([move.submitted, ping, move.completed]);
        assert.equal(joined.status, 'accepted');
        assert.deepEqual(result, { token, surface: 'entry', actorId: 'player' });
        const executions = f.calls.filter((call) => call.token === token);
        assert.equal(executions.length, 1);
        assert.equal(executions[0].surface, 'entry');
        assert.ok(executions[0].correlation);
      }
    } finally {
      await telemetry.observed();
      await f.app.close();
    }
  }
);

test(
  'same-tick Entry to User executes at the target User handler',
  { timeout: 15000 },
  async () => {
    const f = await fixture();
    try {
      const move = f.move(f.roomId);
      const ping = f.ping('user');
      const [, result, joined] = await Promise.all([move.submitted, ping, move.completed]);
      assert.equal(joined.status, 'accepted');
      assert.deepEqual(result, { token: 'user', surface: 'user', actorId: 'player' });
      assert.equal(f.calls.filter((call) => call.token === 'user').length, 1);
    } finally {
      await telemetry.observed();
      await f.app.close();
    }
  }
);

test(
  'source FIFO sends and request preserve sequence at the target after Joined completes',
  { timeout: 15000 },
  async () => {
    const f = await fixture();
    let handler;
    let joined;
    try {
      await completeMove(f, f.roomId);
      handler = f.pauseHandler();
      joined = f.pauseJoined('entry');
      const move = f.move();
      const before = f.notice(1);
      const ping = f.ping('ordered', 2);
      const after = f.notice(3);
      await handler.entered.promise;
      assert.equal(f.calls.length, 0);
      handler.release.resolve();
      await joined.entered.promise;
      assert.equal(f.calls.length, 0);
      joined.release.resolve();
      await Promise.all([move.submitted, move.completed, before, ping, after]);
      assert.deepEqual(
        f.calls.map(({ sequence, surface }) => ({ sequence, surface })),
        [
          { sequence: 1, surface: 'entry' },
          { sequence: 2, surface: 'entry' },
          { sequence: 3, surface: 'entry' }
        ]
      );
    } finally {
      handler?.release.resolve();
      joined?.release.resolve();
      await telemetry.observed();
      await f.app.close();
    }
  }
);

test(
  'an unregistered request reports handler-missing after the move',
  { timeout: 15000 },
  async () => {
    const f = await fixture();
    try {
      await completeMove(f, f.roomId);
      const move = f.move();
      const missing = f.client.requestToActor('player', new Missing()).submit();
      await assert.rejects(missing, /No Spot actor request handler is registered for 'Missing'/);
      await Promise.all([move.submitted, move.completed]);
    } finally {
      await telemetry.observed();
      await f.app.close();
    }
  }
);

test(
  'ingress submitted during Joined stays behind the already accepted source FIFO suffix',
  { timeout: 15000 },
  async () => {
    const f = await fixture();
    let handler;
    let joined;
    try {
      await completeMove(f, f.roomId);
      handler = f.pauseHandler();
      joined = f.pauseJoined('entry');
      const move = f.move();
      const before = f.notice(1);
      const ping = f.ping('suffix', 2);
      const after = f.notice(3);
      await handler.entered.promise;
      handler.release.resolve();
      await joined.entered.promise;
      const newer = f.notice(4);
      await newer.submitted;
      joined.release.resolve();
      await Promise.all([move.submitted, move.completed, before, ping, after, newer]);
      assert.deepEqual(
        f.calls.map((call) => call.sequence),
        [1, 2, 3, 4]
      );
      assert.ok(f.calls.every((call) => call.surface === 'entry'));
    } finally {
      handler?.release.resolve();
      joined?.release.resolve();
      await telemetry.observed();
      await f.app.close();
    }
  }
);

test(
  'forwarded request releases the source SpotWide gate before the target terminal',
  { timeout: 15000 },
  async () => {
    const f = await fixture();
    let held;
    try {
      await completeMove(f, f.roomId);
      await f.actors.getOrCreate('witness', 'player').inMesh('move-ingress').submit();
      const witnessMove = f.move(f.roomId, 'witness');
      await witnessMove.submitted;
      assert.equal((await witnessMove.completed).status, 'accepted');
      held = f.pausePing('held');
      const move = f.move();
      const ping = f.ping('held');
      await held.entered.promise;
      assert.deepEqual(await f.ping('source-progress', 0, 'witness'), {
        token: 'source-progress',
        surface: 'user',
        actorId: 'witness'
      });
      held.release.resolve();
      const [, result, joined] = await Promise.all([move.submitted, ping, move.completed]);
      assert.equal(joined.status, 'accepted');
      assert.equal(result.surface, 'entry');
    } finally {
      held?.release.resolve();
      await telemetry.observed();
      await f.app.close();
    }
  }
);
