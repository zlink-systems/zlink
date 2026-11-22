'use strict';

const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const { setTimeout: delay } = require('node:timers/promises');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');
const telemetry = require('./helpers/telemetry-log-capture');
const { testRedisUrl } = require('./helpers/test-redis');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist');
const {
  ZLinkAutoConnectReconciler
} = require('../../packages/framework/dist/runtime/locations/auto-connect-reconciler');

class Notice {
  constructor(value) {
    this.value = value;
  }
}

class Ping extends Notice {}

async function waitFor(predicate, message) {
  const deadline = Date.now() + 4000;
  while (!predicate()) {
    if (Date.now() >= deadline) assert.fail(typeof message === 'function' ? message() : message);
    await delay(5);
  }
}

test('RouteMesh recovery retains only peers absent from the live descriptor scan', async () => {
  const local = {
    autoConnectType: framework.ZLinkLocationAutoConnectType.RouteMesh,
    meshName: 'fixed-rid-restart',
    role: framework.ZLinkLocationRole.Router,
    nodeRid: 'game',
    endpoint: 'tcp://127.0.0.1:5001'
  };
  const shared = { ...local, nodeRid: 'shared', endpoint: 'tcp://127.0.0.1:5002' };
  const absent = { ...local, nodeRid: 'standby', endpoint: 'tcp://127.0.0.1:5003' };
  let rows = [
    { ...shared, generation: 7n, ownerId: 'previous-owner', draining: false },
    { ...absent, generation: 3n, ownerId: 'other-owner', draining: false }
  ];
  let failed = false;
  let expected;
  let disconnected;
  const connected = [];
  const reconciler = new ZLinkAutoConnectReconciler({
    local,
    peerResolver: {
      async listLivePeers() {
        if (failed) throw new Error('Store unavailable');
        return rows;
      }
    },
    executor: {
      onDisconnected(handler) {
        disconnected = handler;
      },
      connect(target) {
        connected.push(target.lifecycleGeneration);
        return true;
      },
      disconnect() {},
      expectPeers(targets) {
        expected = targets.map(({ nodeRid, lifecycleGeneration }) => [
          nodeRid,
          lifecycleGeneration
        ]);
      }
    }
  });
  await reconciler.tick();
  failed = true;
  await reconciler.tick();
  assert.equal(reconciler.storeFailed, true);
  disconnected(shared.endpoint);
  connected.length = 0;

  failed = false;
  rows = [{ ...shared, generation: 5n, ownerId: 'replacement-owner', draining: false }];
  await reconciler.tick();
  assert.deepEqual(
    { expected, connected },
    {
      expected: [
        ['standby', 3n],
        ['shared', 5n]
      ],
      connected: [5n]
    }
  );
});

test(
  'RouteMesh follows each descriptor published for a restarted fixed RID',
  { timeout: 120000 },
  async (t) => {
    const clock = fixedRidHostClock(t);
    const keyPrefix = `fixed-rid-${randomUUID()}`;
    const inMemory = new framework.ZLinkInMemoryProviderLocationStore();
    const openStore = () =>
      process.env.ZLINK_NODE_FIXED_RID_REDIS === '1'
        ? new ZLinkRedisLocationStore({ url: testRedisUrl, keyPrefix })
        : inMemory;
    const mesh = 'fixed-rid-restart';
    async function start(rid, bind) {
      const notices = [];
      const NoticeHandler = class Notice {
        handle(message) {
          notices.push(message.value);
        }
      };
      const PingHandler = class Ping {
        handle(message) {
          return { value: message.value };
        }
      };
      const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
        builder.addLocationStore(openStore());
        builder.configureLocations().ownerLeaseFencingMarginMs(1).pollingIntervalMs(20);
        builder.configureDispatch().messageFlow('normal');
        const route = builder.addRouteMesh(mesh).listen(bind).routingId(rid);
        route.addRouteSendHandler(NoticeHandler);
        route.addRouteRequestHandler(PingHandler);
      });
      const host = new framework.ZLinkFrameworkRuntimeHost({
        registration,
        providerResolver: { resolve: (type) => new type() }
      });
      try {
        await host.start();
        await finishFixedRidHostStartup(
          host,
          clock,
          framework.ZLinkFrameworkRuntimeState.Preparing
        );
        const node = host.requirePrimaryMeshNode();
        assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
        return {
          host,
          node,
          notices,
          client: new framework.DefaultZLinkRouteClient(
            registration,
            host.routeTransport,
            host.spotRouterChannelIdForMesh
          )
        };
      } catch (error) {
        await host.stop();
        throw error;
      }
    }
    let game;
    let shared;
    try {
      game = await start('game', 'tcp://127.0.0.1:0');
      const descriptorStore = game.host.locationOwner.currentStores.locationStore;
      let previous;
      let bind = 'tcp://127.0.0.1:0';
      const restarts = process.env.ZLINK_NODE_FIXED_RID_RESTARTS === '40' ? 40 : 12;
      for (let iteration = 0; iteration <= restarts; iteration++) {
        shared = await start('shared', bind);
        bind = shared.node.status().localEndpoint;
        const generation = shared.node.status().lifecycleGeneration;
        assert.notEqual(generation, previous);
        previous = generation;

        const descriptors = (await descriptorStore.listMeshNodes(mesh)).items;
        assert.equal(descriptors.length, 2);
        const published = descriptors.find(({ rid }) => String(rid) === 'shared');
        assert.equal(published.lifecycleGeneration, generation);
        const owner = await descriptorStore.readOwnerLease(published.ownerId);
        assert.equal(owner.kind, 'found');
        assert.equal(owner.token.leaseGeneration, published.leaseGeneration);

        await waitFor(
          () =>
            game.node.observePeerRoute('shared').expectation?.lifecycleGeneration === generation,
          () =>
            `restart ${iteration}: game expected ${game.node.observePeerRoute('shared').expectation?.lifecycleGeneration}, Store published ${generation}`
        );
        const startedAdmissionWait = Date.now();
        const readinessBudget = framework.DEFAULT_REQUEST_TIMEOUT_MS;
        const readinessDeadline = BigInt(startedAdmissionWait + readinessBudget);
        const readinessSignal = AbortSignal.timeout(readinessBudget);
        await Promise.all([
          game.node.waitForPeerRoute('shared', generation, readinessDeadline, readinessSignal),
          shared.node.waitForPeerRoute(
            'game',
            game.node.status().lifecycleGeneration,
            readinessDeadline,
            readinessSignal
          )
        ]);
        const admissionMs = Date.now() - startedAdmissionWait;
        for (const [source, destination, rid] of [
          [game, shared, 'shared'],
          [shared, game, 'game']
        ]) {
          const value = `${iteration}:${rid}`;
          await source.client.sendToNode(mesh, rid, new Notice(value)).submit();
          assert.deepEqual(
            await source.client.requestToNode(mesh, rid, new Ping(value)).timeout(1000).submit(),
            { value }
          );
          await waitFor(
            () => destination.notices.includes(value),
            `restart ${iteration}: received ${value}`
          );
        }
        t.diagnostic(
          `restart ${iteration}: descriptor rows=${descriptors.length}, owner=${published.ownerId}, expected=${generation}, ADMIT wait=${admissionMs}ms, both send/request OK`
        );
        await shared.host.stop();
        shared = undefined;
        assert.equal((await descriptorStore.readOwnerLease(published.ownerId)).kind, 'missing');
      }
      await telemetry.observed();
    } finally {
      await shared?.host.stop();
      await game?.host.stop();
    }
  }
);
