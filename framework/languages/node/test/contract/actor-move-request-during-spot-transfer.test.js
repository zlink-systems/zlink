'use strict';

require('reflect-metadata');

const assert = require('node:assert/strict');
const test = require('node:test');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

function pause() {
  const entered = deferred();
  const release = deferred();
  return {
    entered: entered.promise,
    release: release.resolve,
    wait() {
      entered.resolve();
      return release.promise;
    }
  };
}

class Move {
  constructor(target) {
    this.target = target;
  }
}

class Ping {
  constructor(token) {
    this.token = token;
  }
}

class Probe {
  constructor(token) {
    this.token = token;
  }
}

class HoldSource {}

const actorId = 'player';

async function fixture() {
  let client;
  let joinCompleted;
  let transfer;

  class Factory {
    async create(context) {
      return {
        context,
        onJoinCompleted(result) {
          joinCompleted.resolve(result);
        }
      };
    }
  }

  class MoveHandler {
    async handle(_spot, actor, _context, message) {
      actor.context.joinSpot(message.target, {}).defer();
    }
  }

  class PingHandler {
    async handle(spot, actor, context, message) {
      const { token } = message;
      const reply = { token, spotId: spot.context.spotId, actorId: actor.context.actorId };
      transfer.replies.push({ reply, correlationId: context.correlationId });
      return reply;
    }
  }

  class ProbeHandler {
    handle(_spot, message) {
      const request = client.requestToActor(actorId, new Ping(message.token)).submit();
      const probe = transfer.probes.get(message.token);
      request.admitted.then(probe.admitted.resolve, () => {});
      request.then(
        (reply) => probe.terminal.resolve({ reply }),
        (error) => probe.terminal.resolve({ error })
      );
      // Release Spot C so it can submit another request while this reply is pending.
      return {};
    }
  }

  class HoldSourceHandler {
    async handle() {
      await transfer.holdSource.wait();
      return {};
    }
  }

  class Entry {
    configure() {
      this.context.handlers.addHandler(MoveHandler);
    }
    async onLeaveActor() {}
  }

  class Room {
    configure() {
      this.context.handlers
        .addHandler(MoveHandler)
        .addHandler(PingHandler)
        .addPacket(ProbeHandler)
        .addPacket(HoldSourceHandler);
    }
    async onActorJoin() {
      if (this.context.spotId === transfer?.target) await transfer.join.wait();
      return { accepted: true };
    }
    async onJoinedActor() {
      if (this.context.spotId === transfer?.target) transfer.joined.resolve();
    }
    async onLeaveActor() {
      if (this.context.spotId === transfer?.source) await transfer.leave.wait();
    }
    async onClosing() {
      if (this.context.spotId === transfer?.source) await transfer.close.wait();
    }
  }

  const providers = [Factory, Entry, MoveHandler, PingHandler, ProbeHandler, HoldSourceHandler];
  for (const provider of providers) Injectable()(provider);
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
  framework.ZLinkPacket('Probe')(ProbeHandler);
  framework.ZLinkPacket('HoldSource')(HoldSourceHandler);

  const mesh = 'spot-transfer';
  const builder = nest.zlinkFramework();
  if (process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR) builder.configureDispatch().messageFlow('normal');
  builder
    .addRouteMesh(mesh)
    .listen('tcp://127.0.0.1:0')
    .setRoutingIdPrefix(mesh)
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
    providers: [...providers, Room]
  })(AppModule);

  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  try {
    client = app.get(nest.ZLINK_ACTOR_CLIENT);
    const actors = app.get(nest.ZLINK_ACTOR_MANAGER);
    const spots = app.get(nest.ZLINK_SPOT_MANAGER);
    const outbound = app.get(nest.ZLINK_SPOT_OUTBOUND);
    await actors.getOrCreate(actorId, 'player').inMesh(mesh).submit();
    return {
      app,
      async createRoom() {
        return (await spots.create('room').inMesh(mesh).submit()).spot;
      },
      close: (source) => spots.close(source),
      holdSource: (source) => outbound.requestToSpot(source.spotId, new HoldSource()).submit(),
      move(target) {
        joinCompleted = deferred();
        const submitted = client.sendToActor(actorId, new Move(target)).submit();
        return { submitted, completed: joinCompleted.promise };
      },
      beginTransfer(source, target) {
        transfer = {
          source: source.spotId,
          target: target.spotId,
          join: pause(),
          joined: deferred(),
          holdSource: pause(),
          leave: pause(),
          close: pause(),
          probes: new Map(),
          replies: []
        };
        return transfer;
      },
      probe(requester, token) {
        const admitted = deferred();
        const terminal = deferred();
        transfer.probes.set(token, { admitted, terminal });
        outbound
          .requestToSpot(requester.spotId, new Probe(token))
          .submit()
          .catch((error) => {
            terminal.resolve({ error });
          });
        return { admitted: admitted.promise, terminal: terminal.promise, token };
      }
    };
  } catch (error) {
    await app.close();
    throw error;
  }
}

async function reachBoundary(boundary, terminal) {
  await Promise.race([
    boundary,
    terminal.then((result) => {
      if ('error' in result) throw result.error;
      assert.fail('request completed before the expected boundary');
    })
  ]);
}

test(
  '#5351 Spot C requests reach Spot B throughout a Spot transfer',
  { timeout: 60000 },
  async (t) => {
    const f = await fixture();
    try {
      const target = await f.createRoom();
      const requester = await f.createRoom();
      for (let iteration = 0; iteration < 50; iteration++) {
        await t.test(`move ${iteration + 1}`, async () => {
          const source = await f.createRoom();
          const setup = f.move(source.spotId);
          await setup.submitted;
          assert.equal((await setup.completed).status, 'accepted');

          const transfer = f.beginTransfer(source, target);
          const requests = [];
          const probe = (phase) => {
            const request = f.probe(requester, `${phase}-${iteration}`);
            requests.push(request);
            return request;
          };
          let hold;
          let close;
          try {
            const move = f.move(target.spotId);
            await move.submitted;
            await transfer.join.entered;

          // Keep the request queued at A until B commits the Actor membership.
            hold = Promise.allSettled([f.holdSource(source)]);
            await transfer.holdSource.entered;
            const moving = probe('move');
            await reachBoundary(moving.admitted, moving.terminal);
            transfer.join.release();
            await transfer.joined.promise;
            transfer.holdSource.release();

            await reachBoundary(transfer.leave.entered, moving.terminal);
            const leaving = probe('leave');
            await reachBoundary(leaving.admitted, leaving.terminal);
            transfer.leave.release();
            assert.equal((await move.completed).status, 'accepted');

            close = Promise.allSettled([f.close(source)]);
            await transfer.close.entered;
            const closing = probe('close');
            await reachBoundary(closing.admitted, closing.terminal);
            transfer.close.release();
            assert.deepEqual(await close, [{ status: 'fulfilled', value: true }]);
            probe('closed');

            for (const { token, terminal } of requests) {
              const result = await terminal;
              if ('error' in result) throw result.error;
              const reply = { token, spotId: target.spotId, actorId };
              assert.deepEqual(result.reply, reply);
              const executions = transfer.replies.filter((call) => call.reply.token === token);
              assert.equal(executions.length, 1);
              assert.deepEqual(executions[0].reply, reply);
              assert.ok(executions[0].correlationId);
            }
            assert.deepEqual(await hold, [{ status: 'fulfilled', value: {} }]);
          } finally {
            transfer.join.release();
            transfer.holdSource.release();
            transfer.leave.release();
            transfer.close.release();
            await Promise.all(requests.map(({ terminal }) => terminal));
            await hold;
            await close;
          }
        });
      }
    } finally {
      await f.app.close();
    }
  }
);
