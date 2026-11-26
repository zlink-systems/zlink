const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nest = require('../../packages/nestjs/dist');
const { ZLinkFrameworkErrorKind, ZLinkPeerState } = require('../../packages/framework/dist');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/internal');
const {
  M6bServiceWireCommand
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const telemetry = require('./helpers/telemetry-log-capture');
const deferred = require('./helpers/deferred.cjs');
const { setTimeout: delay } = require('node:timers/promises');
const {
  SERVICE_WIRE_COMMAND_OFFSET
} = require('../../packages/framework/dist/runtime/foundation/service-wire-binary-primitives');

const mesh = 'remote-destroy';
const actorId = 'remote-player';
const authorityKey = { value: 'authority\0actor\0' + actorId };

async function startActors(t) {
  const store = new ZLinkInMemoryProviderLocationStore();
  class Entry {
    constructor(context) {
      this.context = context;
    }
    configure() {}
  }
  class Factory {
    async create(context) {
      return { context };
    }
  }
  async function start(rid, owner, endpoint) {
    const builder = nest.zlinkFramework().addLocationStore(store);
    builder.configureDispatch().messageFlow('normal');
    builder
      .configureLocations()
      .pollingIntervalMs(20)
      .ownerLeaseRenewIntervalMs(100)
      .ownerLeaseTtlMs(5000)
      .ownerLeaseFencingMarginMs(500)
      .ownerLeaseRenewTimeoutMs(500);
    const route = builder.addRouteMesh(mesh).listen('tcp://127.0.0.1:0').routingId(rid);
    if (owner) {
      route
        .objects()
        .server()
        .addEntrySpot(Entry, 'player')
        .addActorFactory('player', Factory, (factory) => factory.disableRelocation());
    } else {
      route.objects().client();
      route.peerConnections().connect('owner', endpoint);
    }
    class AppModule {}
    Module({
      imports: [nest.ZLinkModule.forRoot(builder.build())],
      providers: owner ? [Factory] : []
    })(AppModule);
    const app = await NestFactory.createApplicationContext(AppModule, {
      logger: false,
      abortOnError: false
    });
    t.after(() => app.close());
    return app;
  }
  await telemetry.observed();
  const owner = await start('owner', true);
  const query = owner.get(nest.ZLINK_LOCATION_RUNTIME_QUERY);
  let descriptor;
  for (let attempt = 0; attempt < 250; attempt++) {
    descriptor = (await query.listMeshNodeDescriptors(mesh)).items.find(
      (item) => String(item.rid) === 'owner' && item.state === 1
    );
    if (descriptor !== undefined) break;
    await delay(20);
  }
  assert.ok(descriptor, 'owner must publish its Serving descriptor');
  const caller = await start('caller', false, descriptor.endpoint);
  for await (const { status } of caller
    .get(nest.ZLINK_ROUTE_MESH_RUNTIME)
    .observe(mesh, 64, AbortSignal.timeout(5000))) {
    if (
      status.peers.some(
        (peer) => String(peer.nodeRid) === 'owner' && peer.state === ZLinkPeerState.Ready
      )
    ) {
      break;
    }
  }
  const node = (app) => app.get(nest.ZLINK_FRAMEWORK_RUNTIME).spotNodeRuntime.meshNode(mesh);
  const raw = node(caller).runtime;
  const ownerStateful = node(owner).stateful;
  assert.equal(raw.isPeerRouteReady('owner', ownerStateful.nodeGeneration), true);
  const actors = caller.get(nest.ZLINK_ACTOR_MANAGER);
  const created = await actors.create(actorId, 'player').inMesh(mesh).submit();
  assert.equal(created.status, 'created');
  assert.equal(String(created.actor.nodeRid), 'owner');
  return {
    actors,
    ref: created.actor,
    raw,
    ownerStateful,
    ownerHost: owner.get(nest.ZLINK_FRAMEWORK_RUNTIME),
    store
  };
}

test('remote destroy without admitted owner is Unavailable', { timeout: 15000 }, async (t) => {
  const { actors, ref, raw, ownerStateful, store } = await startActors(t);
  const readyPeer = raw.topology.readyPeer.bind(raw.topology);
  t.mock.method(raw.topology, 'readyPeer', (rid) => (rid === 'owner' ? undefined : readyPeer(rid)));
  assert.equal(raw.isPeerRouteReady('owner', ownerStateful.nodeGeneration), false);

  const router = raw.requireStarted();
  const submit = t.mock.method(router, 'submitSend');
  const request = t.mock.method(raw, 'requestService');
  const before = await store.read(authorityKey);
  assert.equal(before.kind, 'found');

  const signal = AbortSignal.timeout(1000);
  const result = await actors.destroy(ref, signal).then(
    (value) => ({ value }),
    (error) => ({ kind: error.kind, message: error.message })
  );
  assert.equal(signal.aborted, false, 'destroy must finish before the deadline');
  assert.equal(
    submit.mock.calls.filter(
      ({ arguments: [, parts] }) =>
        parts[0][SERVICE_WIRE_COMMAND_OFFSET] === M6bServiceWireCommand.actorDestroy
    ).length,
    0
  );
  assert.equal(request.mock.callCount(), 0);
  const after = await store.read(authorityKey);
  assert.equal(after.kind, 'found');
  assert.deepEqual(after.value.version, before.value.version);
  assert.deepEqual(after.value.bytes, before.value.bytes);
  assert.equal(result.kind, ZLinkFrameworkErrorKind.Unavailable, JSON.stringify(result));
});

test(
  'remote destroy sends one command without a reply and observes authority removal',
  { timeout: 15000 },
  async (t) => {
    const { actors, ref, ownerStateful, ownerHost, store } = await startActors(t);
    const received = [];
    const failures = [];
    t.after(ownerHost.errorSink.onRuntimeTaskException(({ error }) => failures.push(error)));
    const ingress = ownerStateful.handleIngress.bind(ownerStateful);
    t.mock.method(ownerStateful, 'handleIngress', (record, ...args) => {
      if (record.command === M6bServiceWireCommand.actorDestroy)
        received.push(record.requestSequence);
      return ingress(record, ...args);
    });
    const deletion = deferred();
    const release = deferred();
    const write = store.write.bind(store);
    t.mock.method(store, 'write', async (request, signal) => {
      if (
        request.mutations.some(
          ({ kind, key }) => kind === 'delete' && key.value === authorityKey.value
        )
      ) {
        deletion.resolve();
        await release.promise;
      }
      return write(request, signal);
    });
    const destroy = actors.destroy(ref, AbortSignal.timeout(5000));
    try {
      await Promise.race([
        deletion.promise,
        destroy.then((result) =>
          assert.fail(`Destroy returned ${result} before authority removal.`)
        )
      ]);
      assert.equal((await store.read(authorityKey)).kind, 'found');
    } finally {
      release.resolve();
    }
    assert.equal(await destroy, true);
    assert.deepEqual(received, [undefined]);
    assert.deepEqual(failures, []);
    assert.equal((await store.read(authorityKey)).kind, 'missing');
  }
);
