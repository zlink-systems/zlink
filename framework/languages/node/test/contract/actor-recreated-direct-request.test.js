'use strict';
const telemetry = require('./helpers/telemetry-log-capture');
const fs = require('node:fs');
const path = require('node:path');
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
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const { ServiceRelocationObjectRestoreOwner } = require('../../packages/framework/dist/internal');

test(
  'cached ID send and request reach the recreated Actor and survive target publication',
  { timeout: 60000 },
  async (t) => {
    const run = `${process.pid}-${randomUUID()}`;
    const meshName = `recreated-${run}`;
    const ownerRoutingId = `owner-${run}`;
    const requesterRoutingId = `requester-${run}`;
    const targetRoutingId = `target-${run}`;
    const locationStore = new ZLinkInMemoryProviderLocationStore();
    const relocationStore = new MemoryRelocationStore();
    const apps = [];
    const handled = [];

    class Ask {
      constructor(value = 'ask') {
        this.value = value;
      }
    }
    class Notice {}
    let noticeGeneration;
    const noticed = new Promise((resolve) => {
      noticeGeneration = resolve;
    });
    class Player {
      constructor(context, nodeRid) {
        this.context = context;
        this.nodeRid = nodeRid;
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
      async onActorJoin() {
        return { accepted: true };
      }
      async onJoinedActor() {}
      async onLeaveActor() {}
    }
    class AskHandler {
      async handle(_entry, actor, _context, request) {
        handled.push(request.value);
        return {
          generation: String(actor.context.objectGeneration),
          node: actor.nodeRid
        };
      }
    }
    class NoticeHandler {
      async handle(_entry, actor) {
        noticeGeneration(String(actor.context.objectGeneration));
      }
    }
    Injectable()(Adapter);
    Injectable()(Entry);
    Injectable()(AskHandler);
    Injectable()(NoticeHandler);
    n.zlinkEntrySpotActorSendHandler({
      actor: () => Player,
      entrySpot: () => Entry,
      packetName: 'Notice'
    })(NoticeHandler);
    n.zlinkEntrySpotActorRequestHandler({
      actor: () => Player,
      entrySpot: () => Entry,
      packetName: 'Ask'
    })(AskHandler);

    async function startNode(role, targetEndpoint) {
      const routingId =
        role === 'owner'
          ? ownerRoutingId
          : role === 'requester'
            ? requesterRoutingId
            : role === 'target'
              ? targetRoutingId
              : `follow-${run}`;
      class Factory {
        async create(context) {
          return new Player(context, routingId);
        }
      }
      Injectable()(Factory);
      const builder = n
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
      const mesh = builder.addRouteMesh(meshName).listen('tcp://127.0.0.1:0').routingId(routingId);
      if (role === 'owner' || role === 'target') {
        mesh
          .objects()
          .server()
          .addEntrySpot(Entry, 'player')
          .addActorFactory('player', Factory, (factory) => factory.preserveStateWith(Adapter));
      } else {
        mesh.objects().client();
      }
      if (role !== 'owner') {
        mesh.peerConnections().connect(ownerRoutingId, targetEndpoint);
      }
      class App {}
      Module({
        imports: [n.ZLinkModule.forRoot(builder.build())],
        providers:
          role === 'owner' || role === 'target'
            ? [Factory, Adapter, Entry, AskHandler, NoticeHandler]
            : []
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
          (item) => String(item.rid) === ownerRoutingId && item.state === 1
        );
        if (descriptor) return descriptor;
        await new Promise((resolve) => setTimeout(resolve, 20));
      }
      throw new Error('owner node not published Serving');
    }

    async function waitForOwnerPeer(requester) {
      const routeMesh = requester.get(n.ZLINK_ROUTE_MESH_RUNTIME);
      for await (const snapshot of routeMesh.observe(meshName, 64, AbortSignal.timeout(10000))) {
        if (
          snapshot.status.isReady &&
          snapshot.status.peers.some(
            (peer) =>
              String(peer.nodeRid) === ownerRoutingId &&
              peer.state === framework.ZLinkPeerState.Ready
          )
        )
          return;
      }
      throw new Error('owner peer not Ready');
    }

    t.after(async () => {
      for (const app of apps.reverse()) await app.close();
      if (process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR !== undefined) {
        fs.writeFileSync(
          path.join(process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR, 'records.jsonl'),
          telemetry.records.map((record) => JSON.stringify(record)).join('\n')
        );
      }
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
    await actorClient.sendToActor('alice', new Notice()).submit();
    const sentGeneration = await noticed;
    const second = await actorClient.requestToActor('alice', new Ask('incarnation')).submit();
    assert.equal(second.node, first.node);
    assert.notEqual(second.generation, first.generation);
    assert.equal(sentGeneration, second.generation);

    const followCaller = await startNode('follow', ownerDescriptor.endpoint);
    await waitForOwnerPeer(followCaller);
    const followClient = followCaller.get(n.ZLINK_ACTOR_CLIENT);
    assert.equal(
      (await followClient.requestToActor('alice', new Ask('incarnation')).submit()).generation,
      second.generation
    );
    const target = await startNode('target', ownerDescriptor.endpoint);
    await waitForOwnerPeer(target);

    const open = ServiceRelocationObjectRestoreOwner.prototype.openAdmission;
    let release;
    const publication = new Promise((resolve) => {
      release = resolve;
    });
    let held;
    const committed = new Promise((resolve) => {
      held = resolve;
    });
    ServiceRelocationObjectRestoreOwner.prototype.openAdmission = async function (...args) {
      held();
      await publication;
      return await open.apply(this, args);
    };
    let relocation;
    try {
      relocation = owner.get(n.ZLINK_FRAMEWORK_RUNTIME).relocate({
        mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
        deadlineMs: 5000
      });
      await Promise.race([
        committed,
        relocation.then((result) => {
          assert.equal(result.outcome, framework.ZLinkFrameworkRelocationOutcome.Relocated);
          return committed;
        })
      ]);
      assert.equal(
        String((await target.get(n.ZLINK_ACTOR_MANAGER).find('alice')).nodeRid),
        targetRoutingId
      );
      const direct = target
        .get(n.ZLINK_ACTOR_CLIENT)
        .requestToActor('alice', new Ask('held-direct'))
        .submit();
      const followed = followClient.requestToActor('alice', new Ask('held-followed')).submit();
      const replies = Promise.allSettled([direct, followed]);
      await followClient.sendToActor('alice', new Notice()).submit();
      assert.deepEqual(
        handled.filter((value) => value.startsWith('held-')),
        []
      );
      release();
      for (const [index, reply] of (await replies).entries()) {
        assert.equal(
          reply.status,
          'fulfilled',
          `${index === 0 ? 'direct' : 'followed'}: ${reply.reason?.stack ?? JSON.stringify(reply)}`
        );
        assert.equal(reply.value.generation, second.generation);
        assert.equal(reply.value.node, targetRoutingId);
      }
    } finally {
      release();
      ServiceRelocationObjectRestoreOwner.prototype.openAdmission = open;
      if (relocation !== undefined) await relocation;
    }
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
    runtime.setActorMessageFollowResolver(
      (fence) => fence.actor.generation === previous.ref.generation
    );
    assert.equal(
      runtime.validateActorMessageFence(cached),
      current,
      'a departed route from an earlier incarnation cannot replace the current Ready Actor'
    );
    const departed = {
      ...cached,
      actor: current.ref,
      authorityOwnerGeneration: current.authorityOwnerGeneration
    };
    runtime.discardRelocatedActor(current.ref);
    runtime.setActorMessageFollowResolver(
      (fence) =>
        fence.actor.generation === departed.actor.generation &&
        fence.authorityOwnerGeneration === departed.authorityOwnerGeneration
    );
    assert.deepEqual(runtime.validateActorMessageFence(departed).ref, departed.actor);
    assert.throws(
      () => runtime.validateActorMessageFence({ ...departed, targetNodeGeneration: 4n }),
      ServiceStaleGenerationError
    );
    assert.throws(() =>
      runtime.validateActorMessageFence({
        ...departed,
        authorityOwnerGeneration: departed.authorityOwnerGeneration + 1n
      })
    );
  } finally {
    runtime.close();
  }
});
