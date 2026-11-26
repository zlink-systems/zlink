'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  decodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const deferred = require('./helpers/deferred.cjs');
const telemetry = require('./helpers/telemetry-log-capture');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');

const actorId = 'player';
const sourceSpotId = 'source-room';
const spotId = 'target-room';
const meshName = 'play';

for (const remote of [false, true]) {
  for (const captured of [0, 1]) {
    test(
      `${remote ? 'remote' : 'same-node'} Actor Join orders authority normalization with ${captured} accepted requests`,
      { timeout: 15_000 },
      async (t) => {
        const clock = fixedRidHostClock(t);
        const locations = new ZLinkInMemoryProviderLocationStore();
        const relocations = new MemoryRelocationStore();
        const repository = new ZLinkLocationStoreRepository(locations);
        const key = encodeAuthorityKey('actor', actorId);
        const targetRid = remote ? 'target' : 'source';
        const apps = [];
        const setupJoined = deferred();
        const joinEntered = deferred();
        const releaseJoin = deferred();
        const requestQueued = deferred();
        const handlerEntered = deferred();
        const releaseHandler = deferred();
        const callbackEntered = deferred();
        const callback = deferred();
        const clearStarted = deferred();
        const releaseClear = deferred();
        let replyReceived = false;
        let callbackCount = 0;
        let callbackFinished = false;
        let clearing = false;
        let request;

        const readAuthority = () => repository.readAuthority(key);
        const write = locations.write.bind(locations);
        if (remote) {
          t.mock.method(locations, 'write', async (operation, signal) => {
            if (!clearing) {
              const current = await readAuthority();
              // Store versions are unique across rows in this in-memory provider.
              const version = operation.conditions.find(
                (condition) =>
                  condition.kind === 'version' &&
                  condition.expected.value === current.storeVersion?.value
              );
              const put = operation.mutations.find(
                (mutation) => mutation.kind === 'put' && mutation.key.value === version?.key.value
              );
              if (
                current.relocation?.phase === 'completed' &&
                put !== undefined &&
                JSON.parse(Buffer.from(put.bytes).toString('utf8')).aggregate === undefined
              ) {
                clearing = true;
                clearStarted.resolve({ current, replyReceived });
                await releaseClear.promise;
              }
            }
            return write(operation, signal);
          });
        }

        class Join {
          constructor(spotId) {
            this.spotId = spotId;
          }
        }
        class Probe {
          constructor(value) {
            this.value = value;
          }
        }
        class Player {
          constructor(context) {
            this.context = context;
          }
          async onJoinCompleted(completion) {
            if (String(this.context.spotId) === sourceSpotId) {
              setupJoined.resolve(completion);
              return;
            }
            callbackCount++;
            callbackEntered.resolve();
            const authority = await readAuthority();
            callbackFinished = true;
            callback.resolve({ completion, authority });
          }
        }
        class Factory {
          create(context) {
            return new Player(context);
          }
        }
        class Entry {}
        class Room {
          async onActorJoin() {
            return { accepted: true };
          }
          async onJoinedActor() {}
          async onLeaveActor() {}
        }
        class JoinHandler {
          async handle(_spot, actor, _context, message) {
            actor.context.joinSpot(message.spotId).defer();
            if (message.spotId === sourceSpotId) return;
            joinEntered.resolve();
            await releaseJoin.promise;
          }
        }
        class ProbeHandler {
          async handle(_room, _actor, _context, message) {
            const afterCallback = callbackFinished;
            handlerEntered.resolve({ afterCallback, authority: await readAuthority() });
            await releaseHandler.promise;
            return { value: message.value };
          }
        }
        class MoveHandler extends JoinHandler {}
        nest.zlinkEntrySpotActorSendHandler({
          entrySpot: () => Entry,
          actor: () => Player,
          packetName: 'Join'
        })(JoinHandler);
        nest.zlinkSpotActorSendHandler({
          spot: () => Room,
          actor: () => Player,
          packetName: 'Join'
        })(MoveHandler);
        nest.zlinkSpotActorRequestHandler({
          spot: () => Room,
          actor: () => Player,
          packetName: 'Probe'
        })(ProbeHandler);

        async function start(rid) {
          const builder = nest
            .zlinkFramework()
            .addLocationStore(locations)
            .addRelocationStore(relocations);
          builder.configureDispatch().messageFlow('normal');
          builder.configureLocations().ownerLeaseFencingMarginMs(1);
          const mesh = builder.addRouteMesh(meshName).routingId(rid).listen('tcp://127.0.0.1:0');
          mesh.channel(meshName).server();
          mesh
            .objects()
            .server()
            .addEntrySpot(Entry, 'Player')
            .addActorFactory('Player', Factory, (factory) => factory.recreateOnRelocation())
            .addSpotFactory(rid, Room, (factory) => factory.disableRelocation());
          class Host {}
          Module({
            imports: [nest.ZLinkModule.forRoot(builder.build())],
            providers: [JoinHandler, MoveHandler, ProbeHandler]
          })(Host);
          const app = await NestFactory.createApplicationContext(Host, {
            logger: false,
            abortOnError: false
          });
          apps.push(app);
          await finishFixedRidHostStartup(
            app.get(nest.ZLINK_FRAMEWORK_RUNTIME),
            clock,
            framework.ZLinkFrameworkRuntimeState.Preparing
          );
          return app;
        }

        function assertTarget(authority) {
          assert.equal(authority.kind, 'snapshot');
          const identity = decodeActorAuthorityIdentity(
            authority.payload,
            authority.objectGeneration
          );
          assert.equal(String(identity.actor.nodeRid), targetRid);
          assert.equal(String(identity.spotId), spotId);
        }

        await telemetry.observed();
        try {
          const source = await start('source');
          const created = await source
            .get(nest.ZLINK_ACTOR_MANAGER)
            .create(actorId, 'Player')
            .inMesh(meshName)
            .submit();
          assert.equal(String(created.actor.nodeRid), 'source');
          const client = source.get(nest.ZLINK_ACTOR_CLIENT);
          const sourceRoom = await source
            .get(nest.ZLINK_SPOT_MANAGER)
            .getOrCreate(sourceSpotId, 'source')
            .inMesh(meshName)
            .submit();
          assert.equal(String(sourceRoom.spot.nodeRid), 'source');
          await client.sendToActor(actorId, new Join(sourceSpotId)).submit();
          assert.equal((await setupJoined.promise).status, 'accepted');

          const target = remote ? await start(targetRid) : source;
          if (remote) {
            for (const app of [source, target]) {
              for await (const snapshot of app
                .get(nest.ZLINK_ROUTE_MESH_RUNTIME)
                .observe(meshName, 64, t.signal)) {
                if (snapshot.status.isReady && snapshot.status.readyPeerCount >= 1) break;
              }
            }
          }
          const room = await target
            .get(nest.ZLINK_SPOT_MANAGER)
            .getOrCreate(spotId, targetRid)
            .inMesh(meshName)
            .submit();
          assert.equal(String(room.spot.nodeRid), targetRid);
          await client.sendToActor(actorId, new Join(spotId)).submit();
          await joinEntered.promise;
          if (captured) {
            const runtime = source.get(nest.ZLINK_FRAMEWORK_RUNTIME);
            const activation = runtime.spotManager.activations.resolve(meshName, sourceSpotId);
            const execute = activation.executeActor.bind(activation);
            t.mock.method(activation, 'executeActor', (id, ...args) => {
              const result = execute(id, ...args);
              if (id === actorId) requestQueued.resolve();
              return result;
            });
            request = client
              .requestToActor(actorId, new Probe(71))
              .submit()
              .then((reply) => {
                replyReceived = true;
                return reply;
              });
            void request.catch(() => {});
            await requestQueued.promise;
          }
          releaseJoin.resolve();

          let enteredBeforeClear;
          if (remote && !captured) {
            const { current } = await clearStarted.promise;
            assertTarget(current);
            assert.equal(current.relocation.completion?.acceptedRequests ?? 0n, 0n);
            enteredBeforeClear = await Promise.race([
              callbackEntered.promise.then(() => true),
              delay(150, false)
            ]);
            releaseClear.resolve();
          }
          const { completion, authority } = await callback.promise;
          assert.equal(completion.status, 'accepted');
          assertTarget(authority);
          if (remote && captured) {
            assert.equal(authority.relocation?.phase, 'activated');
            assert.equal(authority.relocation.completion.acceptedRequests, 1n);
          } else if (!remote) {
            assert.equal(authority.relocation, undefined);
          }

          if (captured) {
            const entered = await handlerEntered.promise;
            assert.equal(entered.afterCallback, true);
            assertTarget(entered.authority);
            assert.equal(entered.authority.relocation?.phase, remote ? 'activated' : undefined);
            assert.equal(replyReceived, false);
            if (remote) {
              assert.equal(entered.authority.relocation.completion.acceptedRequests, 1n);
              assert.equal(clearing, false);
            }
            releaseHandler.resolve();
            assert.deepEqual(await request, { value: 71 });
            if (remote) {
              const atClear = await clearStarted.promise;
              assert.equal(atClear.replyReceived, true);
              assert.equal(atClear.current.relocation.completion.completedResults, 1n);
              assert.equal(atClear.current.relocation.completion.pendingDeliveries, 0n);
              releaseClear.resolve();
            }
          }
          for (;;) {
            const steady = await readAuthority();
            if (steady.relocation === undefined) {
              assertTarget(steady);
              break;
            }
            await delay(10, undefined, { signal: t.signal });
          }
          assert.equal(callbackCount, 1);
          if (remote && !captured) {
            assert.deepEqual(
              { enteredBeforeClear, phaseAtCallback: authority.relocation?.phase },
              { enteredBeforeClear: false, phaseAtCallback: undefined },
              'Join completion must observe steady authority after the finishing CAS'
            );
          }
        } finally {
          releaseJoin.resolve();
          releaseHandler.resolve();
          releaseClear.resolve();
          for (const app of apps.reverse()) await app.close();
        }
      }
    );
  }
}

test.after(() => telemetry.close());
