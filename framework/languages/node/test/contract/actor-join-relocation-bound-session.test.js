'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { randomUUID } = require('node:crypto');
const { setTimeout: delay } = require('node:timers/promises');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { metrics } = require('@opentelemetry/api');
const { MeterProvider, MetricReader } = require('@opentelemetry/sdk-metrics');
const n = require('../../packages/nestjs/dist');
const locations = require('../../packages/framework-locations-redis/dist');
const connector = require('../../packages/stream-connector/dist');
const { connectTestRedis, testRedisUrl } = require('./helpers/test-redis');

async function until(read, message) {
  const deadline = Date.now() + 6000;
  while (Date.now() < deadline) {
    const result = await read();
    if (result) return result;
    await delay(10);
  }
  assert.fail(message);
}

class PublicationReader extends MetricReader {
  async onForceFlush() {}
  async onShutdown() {}

  async published(mesh) {
    const { resourceMetrics } = await this.collect();
    return resourceMetrics.scopeMetrics
      .flatMap((scope) => scope.metrics)
      .some(
        (metric) =>
          metric.descriptor.name === 'zlink.relocation.target_resume' &&
          metric.dataPoints.some((point) => point.attributes.mesh_name === mesh)
      );
  }
}

test('cross-node Actor Join publishes the carried Session binding and committed target', async (t) => {
  const redis = await connectTestRedis(t);
  if (redis === undefined) return;
  t.after(() => redis.quit());
  const reader = new PublicationReader();
  const provider = new MeterProvider({ readers: [reader] });
  metrics.setGlobalMeterProvider(provider);
  t.after(async () => {
    metrics.disable();
    await provider.shutdown();
  });

  for (const callbackFails of [false, true]) {
    await t.test(
      callbackFails ? 'completion throws after commit' : 'completion pushes to the Session',
      { timeout: 20000 },
      async () => {
        const mesh = 'join-binding-' + randomUUID();
        const actorId = 'player';
        const targetSpotId = 'target-room';
        let completion;
        let received;
        let probeCalls = 0;
        let resolvePush;
        const push = { promise: new Promise((resolve) => (resolvePush = resolve)) };
        push.resolve = (value) => resolvePush(value);
        const apps = [];
        let client;
        let sourceActors;

        class Player {
          constructor(context, role) {
            this.context = context;
            this.role = role;
          }

          async onJoinCompleted(result) {
            if (this.role !== 'target') return;
            completion = result;
            if (callbackFails) throw new Error('Join completion callback failed after commit');
            try {
              await this.context.boundSession
                .send({ spot: String(this.context.spotId) })
                .packetName('Joined')
                .submit();
              push.resolve({ ok: true });
            } catch (error) {
              push.resolve({ error });
              throw error;
            }
          }
        }
        class PlayerFactory {
          constructor(role) {
            this.role = role;
          }

          async create(context) {
            return new Player(context, this.role);
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
        }
        class Room {
          async onCreate() {
            return { accepted: true };
          }
          async onActorJoin() {
            return { accepted: true };
          }
          async onJoinedActor() {}
        }
        class Move {
          async handle(_spot, actor) {
            actor.context.joinSpot(targetSpotId).defer();
            return { queued: true };
          }
        }
        class Joined {
          static packetName = 'Joined';
        }
        class Probe {
          async handle() {
            probeCalls += 1;
            return { reached: true };
          }
        }
        class SessionFactory {
          async create(context) {
            return {
              context,
              async onDispatch(dispatch, payload) {
                if (dispatch.packetName !== 'Bind') {
                  await dispatch.actor.relay(payload);
                  return;
                }
                await context.actors.bind(await sourceActors.find(actorId));
                await context.client.reply({ bound: true }).submit();
              }
            };
          }
        }
        const providers = [PlayerState, Entry, Room, Move, SessionFactory];
        Injectable()(Probe);
        for (const Type of providers) Injectable()(Type);
        n.zlinkEntrySpotActorRequestHandler({
          actor: () => Player,
          entrySpot: () => Entry,
          packetName: 'Move'
        })(Move);

        n.zlinkSpotActorRequestHandler({
          actor: () => Player,
          spot: () => Room,
          packetName: 'Probe'
        })(Probe);

        async function start(role, sourceEndpoint) {
          const builder = n.zlinkFramework();
          builder.configureDispatch().messageFlow('normal');
          builder.configureLocations().ownerLeaseFencingMarginMs(1).ownerLeaseRenewIntervalMs(100);
          builder.addLocationStore(
            new locations.ZLinkRedisLocationStore({
              url: testRedisUrl,
              keyPrefix: mesh + ':location'
            })
          );
          builder.addRelocationStore(
            new locations.ZLinkRedisRelocationStore({
              url: testRedisUrl,
              keyPrefix: mesh + ':relocation'
            })
          );
          const route = builder
            .addRouteMesh(mesh)
            .listen('tcp://127.0.0.1:0')
            .routingId(mesh + '-' + role);
          route.channel(mesh).server();
          if (sourceEndpoint) route.peerConnections().connect(mesh + '-source', sourceEndpoint);
          const objects = route.objects().server();
          objects.addEntrySpot(Entry, 'Player');
          objects.addActorFactory('Player', PlayerFactory, (factory) =>
            factory.preserveStateWith(PlayerState)
          );
          if (role === 'target') {
            objects.addSpotFactory('Room', Room, (factory) => factory.disableRelocation());
            builder
              .addStreamNode('gateway')
              .bind('ws://127.0.0.1:0')
              .enableActorDispatch()
              .registerSession(SessionFactory);
          }
          class App {}
          Module({
            imports: [n.ZLinkModule.forRoot(builder.build())],
            providers: [
              ...providers,
              { provide: PlayerFactory, useValue: new PlayerFactory(role) },
              ...(role === 'target' ? [Probe] : [])
            ]
          })(App);
          const app = await NestFactory.createApplicationContext(App, {
            logger: false,
            abortOnError: false
          });
          apps.push(app);
          await until(
            () => app.get(n.ZLINK_FRAMEWORK_RUNTIME).status.isReady,
            role + ' node did not start'
          );
          return app;
        }

        try {
          const source = await start('source');
          sourceActors = source.get(n.ZLINK_ACTOR_MANAGER);
          const created = await sourceActors.getOrCreate(actorId, 'Player').inMesh(mesh).submit();
          assert.equal(created.status, 'created');
          const query = source.get(n.ZLINK_LOCATION_RUNTIME_QUERY);
          const descriptor = await until(
            async () =>
              (await query.listMeshNodeDescriptors(mesh)).items.find(
                (item) => String(item.rid) === mesh + '-source'
              ),
            'source descriptor unavailable'
          );
          const target = await start('target', descriptor.endpoint);
          await until(
            () =>
              [source, target].every(
                (app) => app.get(n.ZLINK_ROUTE_MESH_RUNTIME).snapshot(mesh).readyPeerCount > 0
              ),
            'reciprocal RouteMesh admission unavailable'
          );
          const room = await target
            .get(n.ZLINK_SPOT_MANAGER)
            .getOrCreate(targetSpotId, 'Room')
            .inMesh(mesh)
            .submit();
          assert.equal(String(room.spot.nodeRid), mesh + '-target');

          client = connector.zlinkStreamConnectorFactory.create({
            endpoint: target.get(n.ZLINK_FRAMEWORK_RUNTIME).getListenerStatus('stream', 'gateway')
              .endpoint,
            dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
          });
          await client.connect();
          assert.deepEqual(await client.request({}).packetName('Bind').submit(), { bound: true });
          client.actor(actorId).on(Joined, (message) => {
            received = message.payload;
          });
          const actors = source.get(n.ZLINK_ACTOR_CLIENT);
          assert.deepEqual(await actors.requestToActor(actorId, {}).packetName('Move').submit(), {
            queued: true
          });
          const result = await until(() => completion, 'target Join completion did not run');
          assert.equal(result.status, 'accepted');
          assert.equal(String(result.actor.nodeRid), mesh + '-target');

          if (!callbackFails) {
            const submitted = await push.promise;
            if (submitted.error) throw submitted.error;
            assert.deepEqual(await until(() => received, 'Session did not receive Joined'), {
              spot: targetSpotId
            });
          }
          // Target resume is recorded after the Session route commit is submitted and admission opens.
          await until(
            () => reader.published(mesh),
            'target publication did not follow the accepted Join'
          );
          const ref = await target.get(n.ZLINK_ACTOR_MANAGER).find(actorId);
          assert.equal(String(ref.nodeRid), mesh + '-target');
          const probe = client.actor(actorId).request({}).packetName('Probe').timeout(1000);
          if (callbackFails) {
            await assert.rejects(probe.submit(), {
              message: 'Actor Join completion did not release application dispatch.'
            });
            assert.equal(probeCalls, 0);
            assert.equal(client.state, connector.ZlinkStreamConnectionState.Connected);
          } else {
            assert.deepEqual(await probe.submit(), { reached: true });
            assert.equal(probeCalls, 1);
          }
        } finally {
          await client?.close();
          for (const app of apps.reverse()) await app.close();
          const keys = await redis.keys(mesh + ':*');
          if (keys.length > 0) await redis.del(keys);
        }
      }
    );
  }
});
