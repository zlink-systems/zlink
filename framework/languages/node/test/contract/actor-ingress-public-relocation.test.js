'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const net = require('node:net');
const { randomUUID } = require('node:crypto');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { createClient } = require('redis');
const n = require('../../packages/nestjs/dist');
const locations = require('../../packages/framework-locations-redis/dist');
const connector = require('../../packages/stream-connector/dist');
const telemetry = require('./helpers/telemetry-log-capture');
function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
async function until(read, label) {
  const end = Date.now() + 10000;
  while (Date.now() < end) {
    const value = await read();
    if (value) return value;
    await new Promise((done) => setTimeout(done, 10));
  }
  throw new Error(label);
}
async function port() {
  const server = net.createServer();
  await new Promise((done) => server.listen(0, '127.0.0.1', done));
  const value = server.address().port;
  await new Promise((done) => server.close(done));
  return value;
}
class Move {
  constructor(target, pause = false) {
    this.target = target;
    this.pause = pause;
  }
}
class Notice {
  constructor(value) {
    this.value = value;
  }
}
class Ask {
  constructor(value) {
    this.value = value;
  }
}
class Unregistered {}

const redisUrl = process.env.ZLINK_TEST_REDIS_URL ?? 'redis://127.0.0.1:6379';

test(
  'public relocation transfers queued sends, requests and physical disconnect to the current Spot',
  { timeout: 30000 },
  async (t) => {
    const probe = createClient({ url: redisUrl, socket: { reconnectStrategy: false } });
    probe.on('error', () => {});
    try {
      await probe.connect();
    } catch (error) {
      if (probe.isOpen) probe.disconnect();
      t.skip(`Redis is unavailable at ${redisUrl}: ${error.message}`);
      return;
    }
    await probe.quit();
    const suffix = randomUUID();
    const mesh = 'follow-' + suffix;
    const prefix = 'f2799:' + suffix + ':';
    const sourceRid = 'source-' + suffix;
    const targetRid = 'target-' + suffix;
    const actorId = 'alice';
    const entered = deferred(),
      release = deferred(),
      sendEntered = deferred(),
      releaseSend = deferred();
    const completions = [],
      calls = [],
      disconnected = [],
      observed = [];
    const correlations = new Map();
    let sourceActors;
    class Player {
      constructor(context) {
        this.context = context;
      }
      async onJoinCompleted(result) {
        completions.push({ ...result, spot: String(this.context.spotId) });
      }
    }
    class PlayerFactory {
      async create(context) {
        return new Player(context);
      }
    }
    class PlayerState {
      async capture() {
        return new Uint8Array();
      }
      async restore() {}
    }
    class Entry {
      async onCreateActor() {
        return { accepted: true };
      }
      async onJoinedActor() {}
      async onLeaveActor() {}
      async onDisconnectActor() {
        disconnected.push('entry');
      }
    }
    class Room {
      async onCreate() {
        return { accepted: true };
      }
      async onActorJoin() {
        return { accepted: true };
      }
      async onJoinedActor() {}
      async onLeaveActor() {}
      async onDisconnectActor() {
        disconnected.push(String(this.context.spotId));
      }
    }
    class MoveHandler {
      async handle(_spot, actor, _context, request) {
        actor.context.joinSpot(request.target).defer();
        if (request.pause) {
          entered.resolve();
          await release.promise;
        }
        return { accepted: true };
      }
    }
    class NoticeHandler {
      async handle(spot, _actor, _context, message) {
        calls.push({ kind: 'notice', spot: String(spot.context.spotId), value: message.value });
      }
    }
    class AskHandler {
      async handle(spot, _actor, context, message) {
        correlations.set(message.value, context.correlationId);
        calls.push({ kind: 'ask', spot: String(spot.context.spotId), value: message.value });
        return { spot: String(spot.context.spotId), value: message.value };
      }
    }
    class SessionFactory {
      async create(context) {
        return {
          context,
          async onDispatch() {
            await context.actors.bind(await sourceActors.find(actorId));
            await context.client.reply({ bound: true }).submit();
          }
        };
      }
    }
    for (const Type of [
      PlayerFactory,
      PlayerState,
      Entry,
      Room,
      MoveHandler,
      NoticeHandler,
      AskHandler,
      SessionFactory
    ])
      Injectable()(Type);
    n.zlinkEntrySpotActorRequestHandler({
      actor: () => Player,
      entrySpot: () => Entry,
      packetName: 'Move'
    })(MoveHandler);
    n.zlinkSpotActorRequestHandler({ actor: () => Player, spot: () => Room, packetName: 'Move' })(
      MoveHandler
    );
    n.zlinkSpotActorSendHandler({ actor: () => Player, spot: () => Room, packetName: 'Notice' })(
      NoticeHandler
    );
    n.zlinkSpotActorRequestHandler({ actor: () => Player, spot: () => Room, packetName: 'Ask' })(
      AskHandler
    );
    const apps = [];
    let client;
    const wsPort = await port();
    async function start(role, peer) {
      const builder = n.zlinkFramework();
      builder.configureDispatch().messageFlow('normal');
      builder.addLocationStore(
        new locations.ZLinkRedisLocationStore({
          url: redisUrl,
          keyPrefix: prefix + 'location'
        })
      );
      builder.addRelocationStore(
        new locations.ZLinkRedisRelocationStore({
          url: redisUrl,
          keyPrefix: prefix + 'relocation'
        })
      );
      const route = builder
        .addRouteMesh(mesh)
        .listen('tcp://127.0.0.1:0')
        .routingId(role === 'source' ? sourceRid : targetRid);
      route.channel(mesh).server();
      if (peer) route.peerConnections().connect(sourceRid, peer);
      const objects = route.objects().server();
      objects.addEntrySpot(Entry, 'Player');
      objects.addActorFactory('Player', PlayerFactory, (factory) =>
        factory.preserveStateWith(PlayerState)
      );
      objects.addSpotFactory(role === 'source' ? 'SourceRoom' : 'TargetRoom', Room, (factory) =>
        factory.disableRelocation()
      );
      if (role === 'source')
        builder
          .addStreamNode('gateway')
          .bind(`ws://127.0.0.1:${wsPort}`)
          .enableActorDispatch()
          .registerSession(SessionFactory);
      class AppModule {}
      Module({
        imports: [n.ZLinkModule.forRoot(builder.build())],
        providers: [
          PlayerFactory,
          PlayerState,
          Entry,
          Room,
          MoveHandler,
          NoticeHandler,
          AskHandler,
          SessionFactory
        ]
      })(AppModule);
      const app = await NestFactory.createApplicationContext(AppModule, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      return app;
    }
    telemetry.reset();
    try {
      const source = await start('source');
      sourceActors = source.get(n.ZLINK_ACTOR_MANAGER);
      const sourceSpots = source.get(n.ZLINK_SPOT_MANAGER);
      await sourceSpots.getOrCreate('source-room', 'SourceRoom').inMesh(mesh).submit();
      const created = await sourceActors.getOrCreate(actorId, 'Player').inMesh(mesh).submit();
      assert.equal(created.status, 'created');
      const sourceClient = source.get(n.ZLINK_ACTOR_CLIENT);
      await sourceClient.requestToActor(actorId, new Move('source-room')).submit();
      await until(
        () =>
          completions.some((value) => value.status === 'accepted' && value.spot === 'source-room'),
        'initial public Join did not complete'
      );
      const query = source.get(n.ZLINK_LOCATION_RUNTIME_QUERY);
      const descriptor = await until(
        async () =>
          (await query.listMeshNodeDescriptors(mesh)).items.find(
            (value) => String(value.rid) === sourceRid && value.state === 1
          ),
        'source descriptor unavailable'
      );
      const target = await start('target', descriptor.endpoint);
      await until(
        () =>
          source.get(n.ZLINK_ROUTE_MESH_RUNTIME).snapshot(mesh).readyPeerCount > 0 &&
          target.get(n.ZLINK_ROUTE_MESH_RUNTIME).snapshot(mesh).readyPeerCount > 0,
        'reciprocal RouteMesh admission unavailable'
      );
      await target
        .get(n.ZLINK_SPOT_MANAGER)
        .getOrCreate('target-room', 'TargetRoom')
        .inMesh(mesh)
        .submit();
      const targetClient = target.get(n.ZLINK_ACTOR_CLIENT);
      assert.deepEqual(await targetClient.requestToActor(actorId, new Ask('prime')).submit(), {
        spot: 'source-room',
        value: 'prime'
      });
      client = connector.zlinkStreamConnectorFactory.create({
        endpoint: `ws://127.0.0.1:${wsPort}`
      });
      await client.connect();
      assert.deepEqual(await client.request({}).packetName('Bind').submit(), { bound: true });
      const moving = sourceClient.requestToActor(actorId, new Move('target-room', true)).submit();
      const moveTerminal = moving.then(
        (value) => ({ value }),
        (error) => ({ error })
      );
      await entered.promise;
      // Observe FIFO admission without submitting, rerouting or changing a packet.
      const runtime = source.get(n.ZLINK_FRAMEWORK_RUNTIME);
      const activation = runtime.spotManager.activations.resolve(mesh, 'source-room');
      const originalExecute = activation.executeActor.bind(activation);
      let queued = 0,
        entryQueued = 0;
      t.mock.method(activation, 'executeActor', (id, ...args) => {
        const result = originalExecute(id, ...args);
        if (id === actorId) {
          queued++;
          observed.push(
            result.then(
              (value) => ({ value }),
              (error) => ({ error })
            )
          );
        }
        return result;
      });
      const entry = runtime.spotNodeRuntime.entryActivations.get(mesh).spotSerialExecutor;
      const executeEntry = entry.executeActor.bind(entry);
      t.mock.method(entry, 'executeActor', (id, ...args) => {
        const result = executeEntry(id, ...args);
        if (id === actorId) {
          entryQueued++;
          observed.push(
            result.then(
              (value) => ({ value }),
              (error) => ({ error })
            )
          );
        }
        return result;
      });
      await targetClient.sendToActor(actorId, new Notice('boundary')).submit();
      const asked = targetClient.requestToActor(actorId, new Ask('boundary')).submit();
      const askTerminal = asked.then(
        (value) => ({ value }),
        (error) => ({ error })
      );
      await client.close();
      await until(
        () => queued >= 3 || entryQueued >= 3,
        'send, request and physical disconnect did not enter the previous owner FIFO'
      );
      release.resolve();
      const moveResult = await moveTerminal;
      if (moveResult.error) throw moveResult.error;
      const answer = await askTerminal;
      await Promise.all(observed);
      await until(
        () =>
          completions.some((value) => value.status === 'accepted' && value.spot === 'target-room'),
        'public relocation did not complete'
      );
      assert.deepEqual(await targetClient.requestToActor(actorId, new Ask('drain')).submit(), {
        spot: 'target-room',
        value: 'drain'
      });
      assert.deepEqual(
        {
          calls: calls.filter((value) => value.value === 'boundary'),
          response: answer.error ? { error: answer.error.message } : answer.value,
          disconnected
        },
        {
          calls: [
            { kind: 'notice', spot: 'target-room', value: 'boundary' },
            { kind: 'ask', spot: 'target-room', value: 'boundary' }
          ],
          response: { spot: 'target-room', value: 'boundary' },
          disconnected: ['target-room']
        }
      );
      const boundary = telemetry.records.filter((record) =>
        ['Notice', 'Ask', 'zlink.framework.actor.session_disconnected'].includes(record.packetName)
      );
      assert.equal(
        boundary.filter((record) => ['no_handler', 'stale_target'].includes(record.reason)).length,
        0
      );
      for (const packetName of ['Notice', 'Ask', 'zlink.framework.actor.session_disconnected'])
        assert.equal(
          boundary.filter(
            (record) =>
              record.packetName === packetName &&
              record.phase === (packetName === 'Ask' ? 'replied' : 'completed') &&
              record.spotId === 'target-room' &&
              (packetName !== 'Ask' || record.correlationId === correlations.get('boundary'))
          ).length,
          1
        );
      await targetClient.sendToActor(actorId, new Unregistered()).submit();
      await until(
        () =>
          telemetry.records.some(
            (record) => record.packetName === 'Unregistered' && record.reason === 'no_handler'
          ),
        'missing handler diagnostic absent'
      );
      const ref = await sourceActors.find(actorId);
      assert.equal(String(ref.nodeRid), targetRid);
      // Hold the real transport after public route resolution, without replacing
      // the captured message, fence, operation or reply route.
      const submit = runtime.routeTransport.sendToSpot.bind(runtime.routeTransport);
      t.mock.method(runtime.routeTransport, 'sendToSpot', async (...args) => {
        sendEntered.resolve();
        await releaseSend.promise;
        return submit(...args);
      });
      const absent = sourceClient.sendToActor(actorId, new Notice('absent')).submit();
      const absentTerminal = absent.then(
        (value) => ({ value }),
        (error) => ({ error })
      );
      await sendEntered.promise;
      assert.equal(await target.get(n.ZLINK_ACTOR_MANAGER).destroy(ref), true);
      releaseSend.resolve();
      await until(
        () =>
          telemetry.records.some(
            (record) => record.packetName === 'Notice' && record.reason === 'stale_target'
          ),
        'missing Actor diagnostic absent'
      );
      const absentResult = await absentTerminal;
      assert.deepEqual(absentResult, { value: undefined });
    } finally {
      release.resolve();
      releaseSend.resolve();
      if (client) await client.close();
      for (const app of apps.reverse()) await app.close();
      const redis = createClient({
        url: redisUrl,
        socket: { reconnectStrategy: false }
      });
      redis.on('error', () => {});
      await redis.connect();
      const keys = await redis.keys(prefix + '*');
      if (keys.length) await redis.del(keys);
      await redis.quit();
    }
  }
);
