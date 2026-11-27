'use strict';
const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const test = require('node:test');
const { setTimeout: wait } = require('node:timers/promises');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const {
  ZLinkInMemoryProviderLocationStore,
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/internal');
const nest = require('../../packages/nestjs/dist');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const telemetry = require('./helpers/telemetry-log-capture');
const deferred = require('./helpers/deferred.cjs');

async function until(predicate, signal, description) {
  const deadline = performance.now() + 8000;
  while (performance.now() < deadline) {
    const found = await predicate();
    if (found) return found;
    await wait(10, undefined, { signal });
  }
  assert.fail(description);
}

test(
  'shutdown Preserve beats a held prepared Spot aggregate NewOwner',
  { timeout: 30_000 },
  async (t) => {
    await telemetry.observed();
    const mesh = `preserve-${randomUUID()}`;
    const sourceRid = 'source';
    const targetRid = 'target';
    const spotId = 'room';
    const key = encodeAuthorityKey('user_spot', spotId);
    const provider = new ZLinkInMemoryProviderLocationStore();
    const reader = new ZLinkLocationStoreRepository(provider);
    const relocationStore = new MemoryRelocationStore();
    const held = deferred();
    const release = deferred();
    const committed = deferred();
    let preserved;
    let abortedAggregate;
    const apps = [];
    let moving;
    t.after(async () => {
      release.resolve();
      try {
        await moving;
      } finally {
        for (const app of apps.reverse()) await app.close();
      }
    });

    const write = provider.write.bind(provider);
    t.mock.method(provider, 'write', async (request, signal) => {
      const marker = request.mutations.find(
        (mutation) =>
          mutation.kind === 'put' && mutation.key.value.startsWith('zlink:v11:aggregate:')
      );
      if (marker === undefined) return write(request, signal);
      const { state } = JSON.parse(Buffer.from(marker.bytes).toString());
      if (state === 'committed') {
        held.resolve(marker.key);
        await release.promise;
      }
      const result = await write(request, signal);
      if (state === 'aborted' && result.kind === 'applied') {
        // Observe the Preserve result before the source cleans up the terminal records.
        abortedAggregate = await provider.read(marker.key);
        preserved = await reader.readAuthority(key);
      }
      return result;
    });

    class Room {
      onCreate() {
        return { accepted: true };
      }
    }
    Injectable()(Room);
    async function start(rid, peerEndpoint) {
      const builder = nest
        .zlinkFramework()
        .addLocationStore(provider)
        .addRelocationStore(relocationStore);
      builder.configureDispatch().messageFlow('normal');
      builder
        .configureLocations()
        .pollingIntervalMs(20)
        .ownerLeaseRenewIntervalMs(100)
        .ownerLeaseTtlMs(5000)
        .ownerLeaseFencingMarginMs(500)
        .ownerLeaseRenewTimeoutMs(500);
      const route = builder.addRouteMesh(mesh).listen('tcp://127.0.0.1:0').routingId(rid);
      route.channel(mesh).server();
      if (peerEndpoint !== undefined) route.peerConnections().connect(sourceRid, peerEndpoint);
      route
        .objects()
        .server()
        .addSpotFactory('room', Room, (factory) => factory.recreateOnRelocation());
      class App {}
      Module({ imports: [nest.ZLinkModule.forRoot(builder.build())], providers: [Room] })(App);
      const app = await NestFactory.createApplicationContext(App, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      const host = app.get(nest.ZLINK_FRAMEWORK_RUNTIME);
      const query = app.get(nest.ZLINK_LOCATION_RUNTIME_QUERY);
      const descriptor = await until(
        async () =>
          (await query.listMeshNodeDescriptors(mesh)).items.find(
            (item) =>
              String(item.rid) === rid &&
              item.state === framework.ZLinkFrameworkRuntimeState.Serving
          ),
        t.signal,
        `descriptor for ${rid} did not reach Serving`
      );
      return { app, host, descriptor, node: host.spotNodeRuntime.meshNode(mesh) };
    }

    const source = await start(sourceRid);
    await source.app.get(nest.ZLINK_SPOT_MANAGER).getOrCreate(spotId, 'room').inMesh(mesh).submit();
    const target = await start(targetRid, source.descriptor.endpoint);
    await until(
      () =>
        source.node.peers().some((peer) => String(peer.routingId) === targetRid) &&
        target.node.stateful.raw.isPeerRouteReady(sourceRid),
      t.signal,
      'source and target peer route did not become ready'
    );
    const targetStore = target.host.serviceRelocation.options.locationStore();
    const commitAggregate = targetStore.commitAggregate.bind(targetStore);
    t.mock.method(targetStore, 'commitAggregate', async (...args) => {
      const result = await commitAggregate(...args);
      committed.resolve(result);
      return result;
    });
    moving = source.host.relocate({
      mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
      deadlineMs: 15_000
    });
    const markerKey = await Promise.race([
      held.promise,
      moving.then((result) =>
        assert.fail(`relocation ended before target commit: ${JSON.stringify(result)}`)
      )
    ]);
    const record = async (rowKey) =>
      JSON.parse(Buffer.from((await provider.read(rowKey)).value.bytes).toString());
    const capacityKey = { value: `zlink:v11:capacity:${mesh}:${targetRid}` };
    const before = await reader.readAuthority(key);
    assert.equal(before.kind, 'snapshot');
    assert.equal(before.relocation.phase, 'prepared');
    assert.equal((await record(markerKey)).state, 'prepared');
    assert.equal((await record(capacityKey)).pending.spots, 1);

    const shutdown = await source.host.shutdown({ deadlineMs: 1500 });
    assert.equal(
      shutdown.outcome,
      framework.ZLinkFrameworkTerminationOutcome.Stopped,
      require('node:util').inspect(shutdown)
    );
    assert.equal(abortedAggregate.kind, 'found');
    assert.equal(JSON.parse(Buffer.from(abortedAggregate.value.bytes).toString()).state, 'aborted');
    await moving;
    assert.equal((await provider.read(markerKey)).kind, 'missing');
    assert.equal((await record(capacityKey)).pending.spots, 0);
    assert.equal((await record(capacityKey)).active.spots, 0);
    assert.equal(preserved.kind, 'snapshot');
    assert.equal(preserved.relocation.phase, 'aborted');
    assert.notDeepEqual(preserved.storeVersion, before.storeVersion);
    assert.equal(preserved.ownerId, before.ownerId);
    assert.equal(preserved.ownerLeaseGeneration, before.ownerLeaseGeneration);
    assert.equal(preserved.objectGeneration, before.objectGeneration);
    assert.equal(preserved.authorityOwnerGeneration, before.authorityOwnerGeneration);
    assert.deepEqual(preserved.payload, before.payload);
    release.resolve();
    assert.equal((await committed.promise).kind, 'stale');
  }
);
