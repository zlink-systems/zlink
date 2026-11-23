'use strict';

const assert = require('node:assert/strict');
const { performance } = require('node:perf_hooks');
const { Injectable, Module, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nest = require('@zlink-systems/nestjs');

// The Actor, Entry Spot, and Session use the same public setup as the local
// session/Actor perf role. The connector runs in the server's process.
const mesh = 'runtime-cpu';
const actorId = 'player';
const spotId = 'room';
const iterations = Number(process.argv[2] ?? 30000);
const batchSize = 16;
const completed = { actorSend: 0, actorRequest: 0, spotSend: 0, sessionPush: 0 };
let actorContext;
let actorRef;
let receiveBatch;

class ActorNotice {
  value = 1;
}
class ActorQuery {
  value = 2;
}
class SpotNotice {
  value = 3;
}
class SessionPush {
  value = 4;
}
class BindSession {}

class Player {
  constructor(context) {
    this.context = context;
  }
}
class ActorFactory {
  async create(context) {
    actorContext = context;
    return new Player(context);
  }
}
class Entry {
  async onJoinedActor() {}
  async onLeaveActor() {}
}
class Room {
  onCreate() {
    return { accepted: true };
  }
}
class ActorNoticeHandler {
  handle(_spot, _actor, _context, packet) {
    assert.equal(packet.value, 1);
    received('actorSend');
  }
}
class ActorQueryHandler {
  handle(_spot, _actor, _context, packet) {
    return { value: packet.value };
  }
}
class SpotNoticeHandler {
  handle(_spot, packet) {
    assert.equal(packet.value, 3);
    received('spotSend');
  }
}
class SessionFactory {
  async create(context) {
    return {
      context,
      async onDispatch() {
        await context.actors.bindOrGet(actorRef);
        await context.client.reply({ bound: true }).submit();
      },
      async onError(_context, error) {
        receiveBatch?.reject(new Error(error.message));
      }
    };
  }
}

for (const provider of [
  ActorFactory,
  Entry,
  ActorNoticeHandler,
  ActorQueryHandler,
  SpotNoticeHandler,
  SessionFactory
]) {
  Injectable()(provider);
}
Injectable({ scope: Scope.TRANSIENT })(Room);
nest.zlinkEntrySpotActorSendHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: 'ActorNotice'
})(ActorNoticeHandler);
nest.zlinkEntrySpotActorRequestHandler({
  entrySpot: () => Entry,
  actor: () => Player,
  packetName: 'ActorQuery'
})(ActorQueryHandler);
nest.zlinkSpotPacketHandler({ spot: () => Room, packetName: 'SpotNotice' })(SpotNoticeHandler);

function received(name) {
  completed[name]++;
  if (receiveBatch !== undefined && --receiveBatch.remaining === 0) {
    receiveBatch.resolve();
  }
}

async function run() {
  assert(Number.isSafeInteger(iterations) && iterations >= 1);
  const builder = nest.zlinkFramework();
  builder.configureDispatch().messageFlow('off');
  builder
    .addRouteMesh(mesh)
    .listen('tcp://127.0.0.1:0')
    .setRoutingIdPrefix('runtime-cpu')
    .objects()
    .server()
    .addActorFactory('player', ActorFactory, (factory) => factory.disableRelocation())
    .addEntrySpot(Entry, 'player')
    .addSpotFactory('room', Room, (factory) => factory.disableRelocation());
  builder
    .addStreamNode('stream')
    .enableActorDispatch()
    .bind('ws://127.0.0.1:0')
    .registerSession(SessionFactory);

  class AppModule {}
  Module({
    imports: [
      nest.ZLinkModule.forRoot({ ...builder.build(), locations: { useInMemoryStores: true } })
    ],
    providers: [
      ActorFactory,
      Entry,
      Room,
      ActorNoticeHandler,
      ActorQueryHandler,
      SpotNoticeHandler,
      SessionFactory
    ]
  })(AppModule);

  const app = await NestFactory.createApplicationContext(AppModule, {
    logger: false,
    abortOnError: false
  });
  let connector;
  try {
    const actors = app.get(nest.ZLINK_ACTOR_MANAGER);
    const actorClient = app.get(nest.ZLINK_ACTOR_CLIENT);
    const spots = app.get(nest.ZLINK_SPOT_MANAGER);
    const spotOutbound = app.get(nest.ZLINK_SPOT_OUTBOUND);
    const actorResult = await actors.getOrCreate(actorId, 'player').inMesh(mesh).submit();
    assert.equal(actorResult.status, 'created');
    actorRef = actorResult.actor;
    const spotResult = await spots.getOrCreate(spotId, 'room').inMesh(mesh).submit();
    assert.equal(spotResult.state, 'created');

    const { zlinkStreamConnectorFactory, zlinkStreamJsonCodec, ZlinkStreamDispatchMode } =
      await import('@zlink-systems/stream-connector');
    const endpoint = app
      .get(nest.ZLINK_FRAMEWORK_RUNTIME)
      .getListenerStatus('stream', 'stream').endpoint;
    connector = zlinkStreamConnectorFactory.create({
      endpoint,
      codec: zlinkStreamJsonCodec,
      dispatchMode: ZlinkStreamDispatchMode.Immediate
    });
    connector.on('SessionPush', () => received('sessionPush'));
    await connector.connect();
    assert.deepEqual(await connector.request(new BindSession()).submit(), { bound: true });

    const notice = new ActorNotice();
    const query = new ActorQuery();
    const spotNotice = new SpotNotice();
    const push = new SessionPush();

    async function workload(count) {
      for (let sent = 0; sent < count; sent += batchSize) {
        const limit = Math.min(batchSize, count - sent);
        const expected = Object.fromEntries(
          Object.entries(completed).map(([name, value]) => [name, value + limit])
        );
        const delivered = new Promise((resolve, reject) => {
          receiveBatch = { remaining: 3 * limit, resolve, reject };
        });
        const tasks = [delivered];
        for (let i = 0; i < limit; i++) {
          tasks.push(
            actorClient.sendToActor(actorId, notice).submit(),
            actorClient
              .requestToActor(actorId, query)
              .submit()
              .then((reply) => {
                assert.equal(reply.value, 2);
                completed.actorRequest++;
              }),
            spotOutbound.sendToSpot(spotResult.spot.spotId, spotNotice).submit(),
            actorContext.boundSession.send(push).submit()
          );
        }
        await Promise.all(tasks);
        receiveBatch = undefined;
        assert.deepEqual(completed, expected);
      }
    }

    await workload(Math.min(2500, iterations));
    const startMonotonicUs = Number(process.hrtime.bigint() / 1000n);
    const start = performance.now();
    await workload(iterations);
    const elapsedMs = performance.now() - start;
    const endMonotonicUs = Number(process.hrtime.bigint() / 1000n);
    console.log(
      JSON.stringify({
        operationCounts: Object.fromEntries(
          Object.keys(completed).map((name) => [name, iterations])
        ),
        batchSize,
        elapsedMs,
        msgPerSecond: (4000 * iterations) / elapsedMs,
        startMonotonicUs,
        endMonotonicUs
      })
    );
  } finally {
    try {
      await connector?.close();
    } finally {
      await app.close();
    }
  }
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
