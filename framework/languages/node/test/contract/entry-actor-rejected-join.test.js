require('reflect-metadata');
const assert = require('node:assert/strict');
const test = require('node:test');
const { randomUUID } = require('node:crypto');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const n = require('@zlink-systems/nestjs');
const f = require('@zlink-systems/framework');
const locations = require('@zlink-systems/framework-locations-redis');
const { createClient } = require('redis');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider, SimpleLogRecordProcessor } = require('@opentelemetry/sdk-logs');
const fs = require('node:fs');

if (process.env.ZLINK_TEST_FLOW_FILE) {
  const flow = fs.createWriteStream(process.env.ZLINK_TEST_FLOW_FILE);
  const provider = new LoggerProvider({
    processors: [
      new SimpleLogRecordProcessor({
        exporter: {
          export(records, callback) {
            for (const record of records)
              flow.write(`${JSON.stringify({ body: record.body, ...record.attributes })}\n`);
            callback({ code: 0 });
          },
          async shutdown() {},
          async forceFlush() {}
        }
      })
    ]
  });
  logs.setGlobalLoggerProvider(provider);
  test.after(async () => {
    await provider.shutdown();
    flow.end();
  });
}

const redisUrl = process.env.ZLINK_TEST_REDIS_URL ?? 'redis://127.0.0.1:6379';
const timeoutMs = 10_000;

for (const remoteIngress of [false, true]) {
  test(
    `Entry Actor replays its rejected Join reply after ${remoteIngress ? 'relay' : 'native'} ingress`,
    { timeout: 60_000 },
    async (t) => {
      const redis = createClient({ url: redisUrl, socket: { reconnectStrategy: false } });
      redis.on('error', () => {});
      try {
        await redis.connect();
      } catch (error) {
        if (redis.isOpen) redis.disconnect();
        t.skip(`Redis is unavailable at ${redisUrl}: ${error.message}`);
        return;
      }
      const prefix = `entry-rejected-join:${randomUUID()}:`;
      const apps = [];
      t.after(async () => {
        for (const app of apps.reverse()) await app.close();
        const keys = await redis.keys(`${prefix}*`);
        if (keys.length > 0) await redis.del(keys);
        await redis.quit();
      });
      let clients;
      let complete;
      let fail;
      const done = new Promise((resolve, reject) => {
        complete = resolve;
        fail = reject;
      });
      // Observe callback errors even if they precede the sender's admission.
      void done.catch(() => {});
      const events = [];
      class Begin {}
      class Join {}
      class RegionFull {
        constructor() {
          this.reason = 'region-full';
        }
      }
      class Actor {
        constructor(context) {
          this.context = context;
        }
        async onJoinCompleted(completion) {
          events.push(completion.status);
          try {
            if (completion.status === 'rejected') {
              const reply = completion.reply.decode(RegionFull);
              assert(reply instanceof RegionFull);
              assert.equal(reply.reason, 'region-full');
              await clients.sendToActor(this.context.actorId, reply).submit();
              events.push('self-send-submitted');
            } else if (completion.status === 'accepted') {
              complete();
            } else {
              throw new Error(`Join failed: ${completion.kind}`);
            }
          } catch (error) {
            fail(error);
            throw error;
          }
        }
      }
      class Factory {
        async create(context) {
          return new Actor(context);
        }
      }
      class Entry {
        async onJoinedActor() {}
        async onLeaveActor() {}
      }
      class Spot {
        async onActorJoin() {
          return String(this.context.spotId) === 'full'
            ? { accepted: false, reply: new RegionFull() }
            : { accepted: true };
        }
        async onJoinedActor() {}
        async onLeaveActor() {}
        async onDisconnectActor() {}
      }
      class BeginHandler {
        async handle(_entry, actor) {
          actor.context.joinSpot('full', new Join()).defer();
        }
      }
      class NextHandler {
        async handle(_entry, actor, _context, reply) {
          assert.equal(reply.reason, 'region-full');
          events.push('next-handler');
          actor.context.joinSpot('next', new Join()).defer();
        }
      }
      for (const type of [Factory, Entry, Spot, BeginHandler, NextHandler]) Injectable()(type);
      n.zlinkEntrySpotActorSendHandler({
        entrySpot: () => Entry,
        actor: () => Actor,
        packetName: 'Begin'
      })(BeginHandler);
      n.zlinkEntrySpotActorSendHandler({
        entrySpot: () => Entry,
        actor: () => Actor,
        packetName: 'RegionFull'
      })(NextHandler);
      async function start(role) {
        const builder = n.zlinkFramework();
        builder.configureDispatch().messageFlow('normal');
        builder.addLocationStore(
          new locations.ZLinkRedisLocationStore({ url: redisUrl, keyPrefix: `${prefix}loc` })
        );
        builder.addRelocationStore(
          new locations.ZLinkRedisRelocationStore({ url: redisUrl, keyPrefix: `${prefix}rel` })
        );
        const mesh = builder
          .addRouteMesh('world')
          .routingId(`entry-replay-${role}`)
          .listen('tcp://127.0.0.1:0');
        const objects = mesh.objects().server();
        objects.addEntrySpot(Entry, 'Player');
        objects.addActorFactory('Player', Factory, (c) => c.recreateOnRelocation());
        objects.addSpotFactory('Region', Spot, (c) => {
          c.recreateOnRelocation();
          c.executionMode(f.ZLinkUserSpotExecutionMode.SpotWide);
        });
        class Host {}
        Module({
          imports: [n.ZLinkModule.forRoot(builder.build())],
          providers: [Factory, Entry, Spot, BeginHandler, NextHandler]
        })(Host);
        const app = await NestFactory.createApplicationContext(Host, {
          logger: false,
          abortOnError: false
        });
        apps.push(app);
        return app;
      }
      async function ready(app, peers) {
        for await (const observed of app
          .get(n.ZLINK_ROUTE_MESH_RUNTIME)
          .observe('world', 64, AbortSignal.timeout(timeoutMs))) {
          if (
            observed.status.isReady &&
            observed.status.placement.isAvailable &&
            observed.status.readyPeerCount >= peers
          )
            return;
        }
        assert.fail('Mesh observation ended before readiness');
      }
      const source = await start('source');
      await ready(source, 0);
      await source.get(n.ZLINK_SPOT_MANAGER).getOrCreate('next', 'Region').inMesh('world').submit();
      await source.get(n.ZLINK_ACTOR_MANAGER).getOrCreate('p3', 'Player').inMesh('world').submit();
      const target = await start('target');
      await ready(target, 1);
      await ready(source, 1);
      await target.get(n.ZLINK_SPOT_MANAGER).getOrCreate('full', 'Region').inMesh('world').submit();
      clients = source.get(n.ZLINK_ACTOR_CLIENT);
      const timer = setTimeout(
        () =>
          fail(
            new Error(
              `No accepted completion after rejected Join and self-send: ${events.join(', ')}`
            )
          ),
        timeoutMs
      );
      try {
        await (remoteIngress ? target.get(n.ZLINK_ACTOR_CLIENT) : clients)
          .sendToActor('p3', new Begin())
          .submit();
        await done;
        assert.deepEqual(events, ['rejected', 'self-send-submitted', 'next-handler', 'accepted']);
      } finally {
        clearTimeout(timer);
      }
    }
  );
}
