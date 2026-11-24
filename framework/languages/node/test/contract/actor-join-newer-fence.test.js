'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { randomUUID } = require('node:crypto');
const { setTimeout: wait } = require('node:timers/promises');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const telemetry = require('./helpers/telemetry-log-capture');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

for (const request of [false, true]) {
  const operation = request ? 'request' : 'send';
  test(
    `same-node Actor ${operation} with the committed Join fence runs after the Restore backlog`,
    { timeout: 15000 },
    async (t) => {
      const suffix = randomUUID();
      const meshName = `join-${suffix}`;
      const sourceRid = `source-${suffix}`;
      const targetRid = `target-${suffix}`;
      const locationStore = new ZLinkInMemoryProviderLocationStore();
      const relocationStore = new MemoryRelocationStore();
      const moveEntered = deferred();
      const releaseMove = deferred();
      const commitEntered = deferred();
      const releaseCommit = deferred();
      let joinCompleted = deferred();
      const delivered = deferred();
      const handled = [];

      class Move {
        constructor(target, pause = false) {
          this.target = target;
          this.pause = pause;
        }
      }
      class Notice {
        constructor(label) {
          this.label = label;
        }
      }
      class Ask {
        constructor(label = 'direct') {
          this.label = label;
        }
      }
      class Player {
        constructor(context) {
          this.context = context;
        }
        onJoinCompleted(result) {
          joinCompleted.resolve(result);
        }
      }
      class Factory {
        create(context) {
          return new Player(context);
        }
      }
      class Adapter {
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
        async onLeaveActor() {}
      }
      class MoveHandler {
        async handle(_spot, actor, _context, message) {
          actor.context.joinSpot(message.target).defer();
          if (message.pause) {
            moveEntered.resolve();
            await releaseMove.promise;
          }
          return {};
        }
      }
      class RoomMoveHandler extends MoveHandler {}
      class NoticeHandler {
        async handle(spot, _actor, _context, message) {
          handled.push([String(spot.context.spotId), message.label]);
          if (message.label === 'direct') delivered.resolve();
        }
      }
      class TargetNoticeHandler extends NoticeHandler {}
      class AskHandler {
        async handle(spot, _actor, _context, message) {
          handled.push([String(spot.context.spotId), message.label]);
          return { label: message.label };
        }
      }
      for (const Type of [
        Factory,
        Adapter,
        Entry,
        Room,
        MoveHandler,
        RoomMoveHandler,
        NoticeHandler,
        TargetNoticeHandler,
        AskHandler
      ])
        Injectable()(Type);
      nest.zlinkEntrySpotActorRequestHandler({
        actor: () => Player,
        entrySpot: () => Entry,
        packetName: 'Move'
      })(MoveHandler);
      nest.zlinkSpotActorRequestHandler({
        actor: () => Player,
        spot: () => Room,
        packetName: 'Move'
      })(RoomMoveHandler);
      nest.zlinkEntrySpotActorSendHandler({
        actor: () => Player,
        entrySpot: () => Entry,
        packetName: 'Notice'
      })(NoticeHandler);
      nest.zlinkSpotActorSendHandler({
        actor: () => Player,
        spot: () => Room,
        packetName: 'Notice'
      })(TargetNoticeHandler);
      nest.zlinkSpotActorRequestHandler({
        actor: () => Player,
        spot: () => Room,
        packetName: 'Ask'
      })(AskHandler);

      async function start(rid, peer) {
        const builder = nest
          .zlinkFramework()
          .addLocationStore(locationStore)
          .addRelocationStore(relocationStore);
        builder.configureDispatch().messageFlow('normal');
        builder
          .configureLocations()
          .pollingIntervalMs(20)
          .ownerLeaseRenewIntervalMs(100)
          .ownerLeaseTtlMs(5000)
          .ownerLeaseFencingMarginMs(500)
          .ownerLeaseRenewTimeoutMs(500);
        const route = builder.addRouteMesh(meshName).listen('tcp://127.0.0.1:0').routingId(rid);
        route.channel(meshName).server();
        if (peer) route.peerConnections().connect(sourceRid, peer);
        const objects = route.objects().server();
        objects
          .addEntrySpot(Entry, 'player')
          .addActorFactory('player', Factory, (factory) => factory.preserveStateWith(Adapter));
        objects.addSpotFactory(peer ? 'target-room-type' : 'source-room-type', Room, (factory) =>
          factory.disableRelocation()
        );
        class App {}
        Module({
          imports: [nest.ZLinkModule.forRoot(builder.build())],
          providers: [
            Factory,
            Adapter,
            Entry,
            Room,
            MoveHandler,
            RoomMoveHandler,
            NoticeHandler,
            TargetNoticeHandler,
            AskHandler
          ]
        })(App);
        const app = await NestFactory.createApplicationContext(App, {
          logger: false,
          abortOnError: false
        });
        t.after(() => app.close());
        return app;
      }

      await telemetry.observed();
      const source = await start(sourceRid);
      async function servingDescriptor(app, rid) {
        const query = app.get(nest.ZLINK_LOCATION_RUNTIME_QUERY);
        for (let attempt = 0; attempt < 250; attempt++) {
          const descriptor = (await query.listMeshNodeDescriptors(meshName)).items.find(
            (item) => String(item.rid) === rid && item.state === 1
          );
          if (descriptor !== undefined) return descriptor;
          await wait(20);
        }
        assert.fail(`No Serving descriptor for ${rid}`);
      }
      const sourceDescriptor = await servingDescriptor(source, sourceRid);
      await source
        .get(nest.ZLINK_ACTOR_MANAGER)
        .getOrCreate('player', 'player')
        .inMesh(meshName)
        .submit();
      const sourceClient = source.get(nest.ZLINK_ACTOR_CLIENT);
      await source
        .get(nest.ZLINK_SPOT_MANAGER)
        .getOrCreate('source-room', 'source-room-type')
        .inMesh(meshName)
        .submit();
      await sourceClient.requestToActor('player', new Move('source-room')).submit();
      assert.equal((await joinCompleted.promise).status, 'accepted');
      joinCompleted = deferred();
      const target = await start(targetRid, sourceDescriptor.endpoint);
      await servingDescriptor(target, targetRid);
      const targetMesh = target.get(nest.ZLINK_ROUTE_MESH_RUNTIME);
      for await (const snapshot of targetMesh.observe(meshName, 64, AbortSignal.timeout(5000))) {
        if (
          snapshot.status.peers.some(
            (peer) =>
              String(peer.nodeRid) === sourceRid && peer.state === framework.ZLinkPeerState.Ready
          )
        )
          break;
      }
      await target
        .get(nest.ZLINK_SPOT_MANAGER)
        .getOrCreate('target-room', 'target-room-type')
        .inMesh(meshName)
        .submit();

      const targetHost = target.get(nest.ZLINK_FRAMEWORK_RUNTIME);
      const relocation = targetHost.serviceRelocation;
      const progress = relocation.targetProgress.bind(relocation);
      t.mock.method(relocation, 'targetProgress', async (stage, authority, phase, signal) => {
        // #4718: the owner CAS writes `committed`; `activating` is the first
        // target write after commit and still precedes the Restore publish.
        if (phase === 'activating') {
          commitEntered.resolve(authority);
          await releaseCommit.promise;
        }
        return progress(stage, authority, phase, signal);
      });
      const node = targetHost.spotNodeRuntime.meshNode(meshName);
      const native = node.stateful;
      const path = request ? 'submitLocalRequest' : 'handleIngress';
      const receive = native[path].bind(native);
      const received = deferred();
      t.mock.method(native, path, async (...args) => {
        try {
          return await receive(...args);
        } finally {
          if (request || args[1]?.kind === 'actorSend') received.resolve();
        }
      });
      const targetClient = target.get(nest.ZLINK_ACTOR_CLIENT);
      const move = sourceClient.requestToActor('player', new Move('target-room', true)).submit();
      void move.catch(() => {});
      try {
        await Promise.race([
          moveEntered.promise,
          move.then(() => assert.fail('Move did not enter its handler'))
        ]);
        const sourceHost = source.get(nest.ZLINK_FRAMEWORK_RUNTIME);
        const activation = sourceHost.spotManager.activations.resolve(meshName, 'source-room');
        const queued = deferred();
        const execute = activation.executeActor.bind(activation);
        t.mock.method(activation, 'executeActor', (id, ...args) => {
          const result = execute(id, ...args);
          if (id === 'player') queued.resolve();
          return result;
        });
        await sourceClient.sendToActor('player', new Notice('backlog')).submit();
        await queued.promise;
        releaseMove.resolve();
        const authority = await Promise.race([
          commitEntered.promise,
          joinCompleted.promise.then((terminal) =>
            assert.fail(require('node:util').inspect(terminal))
          )
        ]);
        const previous = native.registry.actor('player');
        assert.equal(previous.ref.generation, authority.objectGeneration);
        assert.ok(previous.authorityOwnerGeneration < authority.authorityOwnerGeneration);
        const direct = request
          ? targetClient.requestToActor('player', new Ask()).submit()
          : targetClient.sendToActor('player', new Notice('direct')).submit();
        const result = direct.then(
          (value) => ({ value }),
          (error) => ({ error })
        );
        await received.promise;
        assert.deepEqual(handled, []);
        releaseCommit.resolve();
        const terminal = await result;
        if (terminal.error) throw terminal.error;
        await move;
        assert.equal((await joinCompleted.promise).status, 'accepted');
        if (request) assert.deepEqual(terminal.value, { label: 'direct' });
        else await delivered.promise;
        assert.deepEqual(handled, [
          ['target-room', 'backlog'],
          ['target-room', 'direct']
        ]);

        if (request) {
          // Vary only the fence sent by the native submit port. Dispatch, typed
          // terminals and diagnostics still run through the normal receive path.
          function overrideFence(context, offset) {
            for (const name of ['sendToActor', 'requestToActor']) {
              const submit = node[name].bind(node);
              context.mock.method(node, name, (actor, parts, options, ...rest) =>
                submit(
                  actor,
                  parts,
                  {
                    ...options,
                    routeFence: {
                      ...options.routeFence,
                      authorityOwnerGeneration: authority.authorityOwnerGeneration + offset
                    }
                  },
                  ...rest
                )
              );
            }
          }
          await t.test(
            'newer fence without a target queue reports both failures',
            async (subtest) => {
              overrideFence(subtest, 1n);
              telemetry.reset();
              await assert.rejects(
                targetClient.requestToActor('player', new Ask('no-stage')).submit(),
                (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
              );
              await targetClient.sendToActor('player', new Notice('no-stage')).submit();
              await telemetry.observed();
              const failures = telemetry.records.filter(
                (record) =>
                  record.actorId === 'player' &&
                  record.errorMessage?.includes('no target temporary queue')
              );
              assert.deepEqual(
                failures.map((record) => [record.packetName, record.action]),
                [
                  ['Ask', 'reply_error'],
                  ['Notice', 'drop']
                ]
              );
              assert.ok(failures.every((record) => record.flowId !== undefined));
              assert.ok(!handled.some(([, label]) => label === 'no-stage'));
            }
          );

          await t.test(
            'older fence remains stale and the equal fence still dispatches',
            async (subtest) => {
              overrideFence(subtest, -1n);
              await assert.rejects(
                targetClient.requestToActor('player', new Ask('older')).submit(),
                (error) =>
                  error.kind === framework.ZLinkFrameworkErrorKind.InternalFailure &&
                  /result 105 \(failure code 17\)/.test(error.message)
              );
              await assert.rejects(
                targetClient.sendToActor('player', new Notice('older')).submit()
              );
            }
          );
          assert.deepEqual(await targetClient.requestToActor('player', new Ask('equal')).submit(), {
            label: 'equal'
          });
          assert.ok(!handled.some(([, label]) => label === 'older'));
        }
      } finally {
        releaseMove.resolve();
        releaseCommit.resolve();
      }
    }
  );
}

test.after(() => telemetry.close());
