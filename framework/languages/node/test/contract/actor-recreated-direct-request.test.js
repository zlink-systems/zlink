'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { randomUUID } = require('node:crypto');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ServiceStaleGenerationError
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-registry');
const n = require('../../packages/nestjs/dist');

test(
  'cached ID request reaches the recreated Actor on the same owner',
  { timeout: 60000 },
  async (t) => {
    const run = `${process.pid}-${randomUUID()}`;
    const meshName = `recreated-${run}`;
    const ownerRoutingId = `owner-${run}`;
    const requesterRoutingId = `requester-${run}`;
    const locationStore = new ZLinkInMemoryProviderLocationStore();
    const apps = [];

    class Ask {
      constructor(value = 'ask') {
        this.value = value;
      }
    }
    class Player {
      constructor(context) {
        this.context = context;
      }
    }
    class Factory {
      async create(context) {
        return new Player(context);
      }
    }
    class Entry {
      async onCreateActor() {
        return { accepted: true };
      }
      async onActorJoin() {
        return { accepted: true };
      }
      async onJoinedActor() {}
      async onLeaveActor() {}
    }
    class AskHandler {
      async handle(_entry, actor, _context, _request) {
        return { generation: String(actor.context.objectGeneration), node: ownerRoutingId };
      }
    }
    Injectable()(Factory);
    Injectable()(Entry);
    Injectable()(AskHandler);
    n.zlinkEntrySpotActorRequestHandler({
      actor: () => Player,
      entrySpot: () => Entry,
      packetName: 'Ask'
    })(AskHandler);

    async function startNode(role, targetEndpoint) {
      const routingId = role === 'owner' ? ownerRoutingId : requesterRoutingId;
      const builder = n.zlinkFramework().addLocationStore(locationStore);
      builder.configureDispatch().messageFlow('normal');
      builder
        .configureLocations()
        .pollingIntervalMs(20)
        .ownerLeaseRenewIntervalMs(100)
        .ownerLeaseTtlMs(5000)
        .ownerLeaseFencingMarginMs(500)
        .ownerLeaseRenewTimeoutMs(500);
      const mesh = builder
        .addRouteMesh(meshName)
        .listen('tcp://127.0.0.1:0')
        .routingId(routingId);
      if (role === 'owner') {
        mesh
          .objects()
          .server()
          .addEntrySpot(Entry, 'player')
          .addActorFactory('player', Factory, (factory) => factory.disableRelocation());
      } else {
        mesh.objects().client();
        mesh.peerConnections().connect(ownerRoutingId, targetEndpoint);
      }
      class App {}
      Module({
        imports: [n.ZLinkModule.forRoot(builder.build())],
        providers: role === 'owner' ? [Factory, Entry, AskHandler] : []
      })(App);
      const app = await NestFactory.createApplicationContext(App, {
        logger: false,
        abortOnError: false
      });
      apps.push(app);
      return app;
    }

    async function waitForOwnerDescriptor(owner) {
      const query = owner.get(n.ZLINK_LOCATION_RUNTIME_QUERY);
      const deadline = Date.now() + 10000;
      while (Date.now() < deadline) {
        const descriptor = (await query.listMeshNodeDescriptors(meshName)).items.find(
          (item) =>
            String(item.rid) === ownerRoutingId &&
            item.state === 1
        );
        if (descriptor) return descriptor;
        await new Promise((resolve) => setTimeout(resolve, 20));
      }
      throw new Error('owner node not published Serving');
    }

    async function waitForOwnerPeer(requester) {
      const routeMesh = requester.get(n.ZLINK_ROUTE_MESH_RUNTIME);
      for await (const snapshot of routeMesh.observe(
        meshName,
        64,
        AbortSignal.timeout(10000)
      )) {
        if (
          snapshot.status.isReady &&
          snapshot.status.peers.some(
            (peer) =>
              String(peer.nodeRid) === ownerRoutingId && peer.state === framework.ZLinkPeerState.Ready
          )
        )
          return;
      }
      throw new Error('owner peer not Ready');
    }

    t.after(async () => {
      for (const app of apps.reverse()) await app.close();
    });

    const owner = await startNode('owner');
    const ownerDescriptor = await waitForOwnerDescriptor(owner);
    const actors = owner.get(n.ZLINK_ACTOR_MANAGER);
    await actors.getOrCreate('alice', 'player').inMesh(meshName).submit();

    const requester = await startNode('requester', ownerDescriptor.endpoint);
    await waitForOwnerPeer(requester);
    const actorClient = requester.get(n.ZLINK_ACTOR_CLIENT);
    const routeIsCached = () =>
      requester
        .get(n.ZLINK_FRAMEWORK_RUNTIME)
        [inspectRuntime]()
        .actorRouteCache?.directActorRouteKeys.includes('alice') ?? false;

    const first = await actorClient.requestToActor('alice', new Ask('incarnation')).submit();
    assert.equal(routeIsCached(), true);
    assert.equal(await actors.destroy(await actors.find('alice')), true);
    await actors.getOrCreate('alice', 'player').inMesh(meshName).submit();
    assert.equal(routeIsCached(), true);
    const second = await actorClient.requestToActor('alice', new Ask('incarnation')).submit();
    assert.equal(second.node, first.node);
    assert.notEqual(second.generation, first.generation);
  }
);

test('direct Actor admission rejects an old owner of the same incarnation', () => {
  const runtime = new ServiceStatefulRuntime(
    {
      observePeerRoute: () => ({ generation: 1n, ready: true }),
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved: () => () => {},
      setServiceIngress() {}
    },
    'node-a',
    3n
  );
  try {
    const previous = runtime.createActor('actor-a');
    runtime.discardRelocatedActor(previous.ref);
    const current = runtime.createActor('actor-a');
    const cached = {
      actor: previous.ref,
      targetNodeGeneration: 3n,
      authorityOwnerGeneration: previous.authorityOwnerGeneration,
      ownerLeaseGeneration: 3n
    };
    assert.equal(runtime.validateActorMessageFence(cached), current);
    assert.throws(
      () => runtime.validateActorMessageFence({ ...cached, actor: current.ref }),
      ServiceStaleGenerationError
    );
  } finally {
    runtime.close();
  }
});
