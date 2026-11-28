'use strict';

const assert = require('node:assert/strict');
const { setTimeout: wait } = require('node:timers/promises');
const { inspect } = require('node:util');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../../packages/framework/dist');
const nest = require('../../../packages/nestjs/dist');
const {
  encodeAuthorityKey
} = require('../../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  RELOCATION_REPLY_PREFIX
} = require('../../../packages/framework/dist/runtime/locations/relocation-reply-record');
const { MemoryRelocationStore } = require('./instance-relocation-memory-store');
const telemetry = require('../helpers/telemetry-log-capture');
const deferred = require('../helpers/deferred.cjs');

const timeout = 10_000;
const mesh = 'reply-mesh';
const actorId = 'relocating-request-actor';
const sourceRid = 'reply-source';
const sourceSpotId = 'reply-room';
const targetRid = 'reply-target';
const destinationRid = 'reply-join-destination';

async function waitForPeers(app, peerRids) {
  const route = app.get(nest.ZLINK_ROUTE_MESH_RUNTIME);
  for await (const { status } of route.observe(mesh, 8, AbortSignal.timeout(timeout))) {
    if (
      status.isReady &&
      peerRids.every((rid) =>
        status.peers.some(
          (peer) => String(peer.nodeRid) === rid && peer.state === framework.ZLinkPeerState.Ready
        )
      )
    )
      return;
  }
  assert.fail(`RouteMesh peers did not become ready: ${peerRids.join(', ')}`);
}

const actorKey = encodeAuthorityKey('actor', actorId);

function actorReplyMutation(request) {
  for (const mutation of request.mutations) {
    if (mutation.kind !== 'put' || !mutation.key.value.startsWith(RELOCATION_REPLY_PREFIX))
      continue;
    const row = JSON.parse(Buffer.from(mutation.bytes).toString('utf8'));
    if (row.objectKey === actorKey.value) return { row, key: mutation.key };
  }
  return undefined;
}

async function startCapturedActorRequestRelocation(t, locations, independentRequester) {
  await telemetry.observed();
  const captureStarted = deferred();
  const releaseCapture = deferred();
  const initialJoin = deferred();
  const deferredJoin = deferred();
  const relocations = new MemoryRelocationStore();
  const apps = [];
  t.after(async () => {
    releaseCapture.resolve();
    for (const app of apps.reverse()) await app.close();
  });

  class Join {
    constructor(spotId) {
      this.spotId = spotId;
    }
  }
  class Player {
    constructor(context, rid) {
      this.context = context;
      this.rid = rid;
    }

    async onJoinCompleted(result) {
      (this.rid === sourceRid ? initialJoin : deferredJoin).resolve(result);
    }
  }
  class PlayerState {
    async capture(actor) {
      if (actor.rid === sourceRid && actor.context.actorId === actorId) {
        captureStarted.resolve();
        await releaseCapture.promise;
      }
      return new Uint8Array();
    }

    async restore() {}
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
    handle(_spot, actor, _context, request) {
      actor.context.joinSpot(request.spotId).timeout(timeout).defer();
      return { result: 'scheduled', nodeRid: actor.rid };
    }
  }
  class RelocatedJoinHandler extends JoinHandler {}
  for (const type of [PlayerState, Entry, Room, JoinHandler, RelocatedJoinHandler]) {
    Injectable()(type);
  }
  nest.zlinkEntrySpotActorRequestHandler({
    entrySpot: () => Entry,
    actor: () => Player,
    packetName: 'Join'
  })(JoinHandler);
  nest.zlinkSpotActorRequestHandler({
    spot: () => Room,
    actor: () => Player,
    packetName: 'Join'
  })(RelocatedJoinHandler);

  async function start(rid, peer) {
    class Factory {
      create(context) {
        return new Player(context, rid);
      }
    }
    Injectable()(Factory);
    const builder = nest
      .zlinkFramework()
      .addLocationStore(locations)
      .addRelocationStore(relocations);
    builder.configureDispatch().messageFlow('normal');
    builder
      .configureLocations()
      .pollingIntervalMs(20)
      .ownerLeaseRenewIntervalMs(100)
      .ownerLeaseTtlMs(5000)
      .ownerLeaseFencingMarginMs(500)
      .ownerLeaseRenewTimeoutMs(500);
    const route = builder.addRouteMesh(mesh).listen('tcp://127.0.0.1:0').routingId(rid);
    if (peer !== undefined) route.peerConnections().connect(peer.rid, peer.endpoint);
    const objects = route
      .objects()
      .server()
      .addEntrySpot(Entry, 'player')
      .addActorFactory('player', Factory, (factory) => factory.preserveStateWith(PlayerState));
    if (rid === destinationRid) {
      objects.addSpotFactory('join-only', Room, (factory) => factory.disableRelocation());
    } else {
      objects.addSpotFactory('room', Room, (factory) => factory.recreateOnRelocation());
    }

    class App {}
    Module({
      imports: [nest.ZLinkModule.forRoot(builder.build())],
      providers: [Factory, PlayerState, Entry, Room, JoinHandler, RelocatedJoinHandler]
    })(App);
    const app = await NestFactory.createApplicationContext(App, {
      logger: false,
      abortOnError: false
    });
    apps.push(app);
    const query = app.get(nest.ZLINK_LOCATION_RUNTIME_QUERY);
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      const descriptor = (await query.listMeshNodeDescriptors(mesh)).items.find(
        (item) =>
          String(item.rid) === rid && item.state === framework.ZLinkFrameworkRuntimeState.Serving
      );
      if (descriptor) return { app, rid, endpoint: descriptor.endpoint };
      await wait(20);
    }
    assert.fail(`Host did not publish a Serving descriptor: ${rid}`);
  }

  const source = await start(sourceRid);
  await source.app
    .get(nest.ZLINK_SPOT_MANAGER)
    .getOrCreate(sourceSpotId, 'room')
    .inMesh(mesh)
    .submit();
  await source.app.get(nest.ZLINK_ACTOR_MANAGER).create(actorId, 'player').inMesh(mesh).submit();
  const sourceClient = source.app.get(nest.ZLINK_ACTOR_CLIENT);
  assert.deepEqual(
    await sourceClient.requestToActor(actorId, new Join(sourceSpotId)).timeout(timeout).submit(),
    {
      result: 'scheduled',
      nodeRid: sourceRid
    }
  );
  assert.equal((await initialJoin.promise).status, 'accepted');

  const target = await start(targetRid, source);
  const destination = await start(destinationRid, target);
  await destination.app
    .get(nest.ZLINK_SPOT_MANAGER)
    .getOrCreate('reply-join-only', 'join-only')
    .inMesh(mesh)
    .submit();
  await Promise.all([
    waitForPeers(source.app, independentRequester ? [targetRid, destinationRid] : [targetRid]),
    waitForPeers(destination.app, independentRequester ? [targetRid, sourceRid] : [targetRid])
  ]);

  function relocate() {
    const moving = source.app.get(nest.ZLINK_FRAMEWORK_RUNTIME).relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      deadlineMs: timeout,
      signal: t.signal
    });
    const capturing = Promise.race([
      captureStarted.promise,
      moving.then((result) =>
        assert.fail(`Relocation ended before Actor capture: ${inspect(result)}`)
      )
    ]);
    return { moving, capturing };
  }

  return {
    client: independentRequester ? destination.app.get(nest.ZLINK_ACTOR_CLIENT) : sourceClient,
    query: destination.app.get(nest.ZLINK_LOCATION_RUNTIME_QUERY),
    request: new Join('reply-join-only'),
    relocate,
    releaseCapture,
    deferredJoin
  };
}

module.exports = {
  actorId,
  sourceRid,
  sourceSpotId,
  targetRid,
  destinationRid,
  timeout,
  actorReplyMutation,
  startCapturedActorRequestRelocation
};
