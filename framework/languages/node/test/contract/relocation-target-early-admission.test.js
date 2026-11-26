'use strict';
const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { setTimeout: wait } = require('node:timers/promises');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { RequestResult } = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const nest = require('../../packages/nestjs/dist');
const {
  encodeChannelEnvelopeParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const telemetry = require('./helpers/telemetry-log-capture');

const timeout = 10_000;
const actorId = 'relocating-actor';
const spotId = 'relocating-spot';
const otherActorId = 'other-actor';
const otherSpotId = 'other-spot';

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

async function until(predicate) {
  for (let attempt = 0; attempt < 500; attempt++) {
    const found = await predicate();
    if (found) return found;
    await wait(10);
  }
  assert.fail('Expected runtime transition did not occur.');
}

for (const kind of ['actor', 'user_spot']) {
  test(
    `committed ${kind} admits ordered direct requests while another object makes progress`,
    { timeout: 30_000 },
    async (t) => {
      await telemetry.observed();
      const nonce = randomUUID();
      const mesh = `admission-${nonce}`;
      const sourceRid = `source-${nonce}`;
      const targetRid = `target-${nonce}`;
      const observerRid = `observer-${nonce}`;
      const locationStore = new ZLinkInMemoryProviderLocationStore();
      const relocationStore = new MemoryRelocationStore();
      const join = deferred();
      const committed = deferred();
      const open = deferred();
      const readStarted = deferred();
      const allowRead = deferred();
      const handled = [];
      class Join {}
      class Work {
        constructor(marker) {
          this.marker = marker;
        }
      }
      class Player {
        constructor(context) {
          this.context = context;
        }
        onJoinCompleted(result) {
          join.resolve(result);
        }
      }
      class Factory {
        create(context) {
          return new Player(context);
        }
      }
      class Entry {
        onCreateActor() {
          return { accepted: true };
        }
        onJoinedActor() {}
        onLeaveActor() {}
      }
      class Room {
        onCreate() {
          return { accepted: true };
        }
        onActorJoin() {
          return { accepted: true };
        }
        onJoinedActor() {}
        onLeaveActor() {}
      }
      class JoinHandler {
        handle(_spot, actor) {
          actor.context.joinSpot(spotId).defer();
          return {};
        }
      }
      class ActorWorkHandler {
        handle(_spot, actor, _context, packet) {
          handled.push([actor.context.actorId, packet.marker]);
          return packet;
        }
      }
      class EntryWorkHandler extends ActorWorkHandler {}
      class SpotWorkHandler {
        handle(spot, packet) {
          handled.push([String(spot.context.spotId), packet.marker]);
          return packet;
        }
      }
      const providers = [
        Factory,
        Entry,
        Room,
        JoinHandler,
        ActorWorkHandler,
        EntryWorkHandler,
        SpotWorkHandler
      ];
      for (const type of providers) Injectable()(type);
      nest.zlinkEntrySpotActorRequestHandler({
        entrySpot: () => Entry,
        actor: () => Player,
        packetName: 'Join'
      })(JoinHandler);
      nest.zlinkEntrySpotActorRequestHandler({
        entrySpot: () => Entry,
        actor: () => Player,
        packetName: 'Work'
      })(EntryWorkHandler);
      nest.zlinkSpotActorRequestHandler({
        spot: () => Room,
        actor: () => Player,
        packetName: 'Work'
      })(ActorWorkHandler);
      nest.zlinkSpotPacketHandler({ spot: () => Room, packetName: 'Work' })(SpotWorkHandler);

      const apps = [];
      let moving;
      t.after(async () => {
        open.resolve();
        allowRead.resolve();
        try {
          await moving;
        } finally {
          for (const app of apps.reverse()) await app.close();
        }
      });
      async function start(rid, peerRid, endpoint) {
        const builder = nest
          .zlinkFramework()
          .addLocationStore(locationStore)
          .addRelocationStore(relocationStore);
        builder.configureDispatch().messageFlow('normal');
        builder.configureInboundDispatch().maxQueuedApplicationJobs(1n);
        builder
          .configureLocations()
          .pollingIntervalMs(20)
          .ownerLeaseRenewIntervalMs(100)
          .ownerLeaseTtlMs(5000)
          .ownerLeaseFencingMarginMs(500)
          .ownerLeaseRenewTimeoutMs(500);
        const route = builder.addRouteMesh(mesh).listen('tcp://127.0.0.1:0').routingId(rid);
        route.channel(mesh).server();
        if (peerRid !== undefined) route.peerConnections().connect(peerRid, endpoint);
        if (rid === observerRid) route.objects().client();
        else
          route
            .objects()
            .server()
            .addEntrySpot(Entry, 'player')
            .addActorFactory('player', Factory, (factory) => factory.recreateOnRelocation())
            .addSpotFactory('room', Room, (factory) => factory.recreateOnRelocation());
        class App {}
        Module({
          imports: [nest.ZLinkModule.forRoot(builder.build())],
          providers: rid === observerRid ? [] : providers
        })(App);
        const app = await NestFactory.createApplicationContext(App, {
          logger: false,
          abortOnError: false
        });
        apps.push(app);
        const host = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
        const node = host.spotNodeRuntime.meshNode(mesh);
        const query = app.get(nest.ZLINK_LOCATION_RUNTIME_QUERY);
        const descriptor = await until(async () =>
          (await query.listMeshNodeDescriptors(mesh)).items.find(
            (item) => String(item.rid) === rid && item.state === 1
          )
        );
        return { app, host, node, descriptor, actor: app.get(nest.ZLINK_ACTOR_CLIENT) };
      }
      const source = await start(sourceRid);
      await source.app
        .get(nest.ZLINK_SPOT_MANAGER)
        .getOrCreate(spotId, 'room')
        .inMesh(mesh)
        .submit();
      if (kind === 'actor') {
        await source.app
          .get(nest.ZLINK_ACTOR_MANAGER)
          .getOrCreate(actorId, 'player')
          .inMesh(mesh)
          .submit();
        await source.actor.requestToActor(actorId, new Join()).submit();
        assert.equal((await join.promise).status, 'accepted');
      }
      const target = await start(targetRid, sourceRid, source.descriptor.endpoint);
      await target.app
        .get(nest.ZLINK_SPOT_MANAGER)
        .getOrCreate(otherSpotId, 'room')
        .inMesh(mesh)
        .submit();
      await target.app
        .get(nest.ZLINK_ACTOR_MANAGER)
        .getOrCreate(otherActorId, 'player')
        .inMesh(mesh)
        .submit();
      const observer = await start(observerRid, targetRid, target.descriptor.endpoint);
      await until(
        () =>
          source.node
            .peers()
            .some(
              (peer) =>
                String(peer.routingId) === targetRid &&
                target.node.stateful.raw.isPeerRouteReady(sourceRid) === true
            ) && observer.node.stateful.raw.isPeerRouteReady(targetRid) === true
      );

      const runtime = target.host.serviceRelocation;
      const progress = runtime.targetProgress.bind(runtime);
      t.mock.method(runtime, 'targetProgress', async (stage, authority, phase, signal) => {
        if (phase === 'activating' && stage.staging.hidden.has(key.value)) {
          committed.resolve({ authority, stage });
          await open.promise;
        }
        return progress(stage, authority, phase, signal);
      });
      const key = encodeAuthorityKey(kind, kind === 'actor' ? actorId : spotId);
      const store = runtime.options.locationStore;
      let requestedReads = 0;
      t.mock.method(
        runtime.options,
        'locationStore',
        () =>
          new Proxy(store(), {
            get(inner, property) {
              if (property !== 'readAuthority') {
                const value = Reflect.get(inner, property);
                return typeof value === 'function' ? value.bind(inner) : value;
              }
              return async (requested, signal) => {
                if (requested.value === key.value && requestedReads++ === baselineReads) {
                  readStarted.resolve();
                  await allowRead.promise;
                }
                return inner.readAuthority(requested, signal);
              };
            }
          })
      );
      let baselineReads = -1;
      moving = source.host.relocate({
        mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
        deadlineMs: 15_000
      });
      const { authority, stage } = await Promise.race([
        committed.promise,
        moving.then((result) => {
          assert.equal(
            result.outcome,
            framework.ZLinkFrameworkRelocationOutcome.Relocated,
            require('node:util').inspect(result)
          );
          return committed.promise;
        })
      ]);
      assert.equal(authority.relocation.phase, 'committed');
      const snapshot = await store().readAuthority(key);
      assert.equal(snapshot.kind, 'snapshot');
      assert.equal(String(snapshot.allocation.descriptor.rid), targetRid);
      baselineReads = requestedReads;
      function spotRequest(sender, id, to, marker, offset = 0n) {
        const parts = encodeChannelEnvelopeParts(1, mesh, 'Work', new Work(marker), timeout);
        return new Promise((resolve) => {
          sender.node.requestFromSpot(
            { ref: { spotId: String(sender.descriptor.rid) } },
            {
              spot: {
                spotId: id,
                generation: to.objectGeneration
              },
              targetNodeRid: String(to.allocation.descriptor.rid),
              targetNodeGeneration: to.allocation.descriptorLifecycleGeneration,
              ownerLeaseGeneration: to.ownerLeaseGeneration,
              authorityOwnerGeneration: to.authorityOwnerGeneration + offset,
              storeVersion: to.storeVersion.value
            },
            parts,
            timeout,
            undefined,
            undefined,
            (result, materialize) =>
              resolve(
                offset === 0n
                  ? result.terminalResult
                  : {
                      terminal: result.terminalResult,
                      code: materialize().failureErrno
                    }
              )
          );
        });
      }
      const ask = (sender, marker) =>
        kind === 'actor'
          ? sender.actor
              .requestToActor(actorId, new Work(marker))
              .timeout(timeout)
              .submit()
              .then((response) => response.marker)
          : spotRequest(sender, spotId, snapshot, marker);
      try {
        const first = ask(target, 'first');
        const firstTerminal = first.then((value) =>
          assert.fail(`Request completed before dispatch opening: ${value}`)
        );
        await Promise.race([readStarted.promise, firstTerminal]);
        const second = ask(target, 'second');
        const remoteFirst = ask(observer, 'remote-first');
        const remoteSecond = ask(observer, 'remote-second');
        for (const result of [second, remoteFirst, remoteSecond]) void result.catch(() => {});
        if (kind === 'actor') {
          assert.deepEqual(
            await observer.actor.requestToActor(otherActorId, new Work('unrelated')).submit(),
            { marker: 'unrelated' }
          );
        } else {
          const other = await store().readAuthority(encodeAuthorityKey('user_spot', otherSpotId));
          assert.equal(
            await spotRequest(observer, otherSpotId, other, 'unrelated'),
            RequestResult.Ok
          );
        }
        await until(() => requestedReads >= baselineReads + 4);
        const stale =
          kind === 'user_spot'
            ? await Promise.all([
                spotRequest(target, spotId, snapshot, 'stale-local', 1n),
                spotRequest(observer, spotId, snapshot, 'stale-remote', 1n)
              ])
            : undefined;
        if (stale !== undefined)
          assert.deepEqual(stale, [
            { terminal: RequestResult.NotFound, code: 14 },
            { terminal: RequestResult.Conflict, code: 33 }
          ]);
        assert.deepEqual(handled, [[kind === 'actor' ? otherActorId : otherSpotId, 'unrelated']]);
        open.resolve();
        await until(() => !runtime.targetStages.has(stage.staging.id));
        assert.deepEqual(handled, [[kind === 'actor' ? otherActorId : otherSpotId, 'unrelated']]);
        allowRead.resolve();
        assert.deepEqual(
          await Promise.all([first, second, remoteFirst, remoteSecond]),
          kind === 'actor'
            ? ['first', 'second', 'remote-first', 'remote-second']
            : Array(4).fill(RequestResult.Ok)
        );
        const labels = handled.slice(1).map(([, marker]) => marker);
        assert.equal(labels[0], 'first');
        assert.ok(labels.indexOf('second') > labels.indexOf('first'));
        assert.ok(labels.indexOf('remote-first') > 0);
        assert.ok(labels.indexOf('remote-second') > labels.indexOf('remote-first'));
        assert.equal(labels.length, 4);
        assert.equal((await moving).outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
      } finally {
        allowRead.resolve();
        open.resolve();
      }
    }
  );
}
