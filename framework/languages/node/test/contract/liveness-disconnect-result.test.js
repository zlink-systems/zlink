const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const zlink = require('@zlink-systems/zlink');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ZLinkNodeRawBindingPort
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  M6A_SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

const {
  DEFAULT_SERVICE_PEER_TIMEOUT_MS
} = require('../../packages/framework/dist/runtime/foundation/service-liveness-registry');

function peer(rid, context, clock) {
  let router;
  const port = new ZLinkNodeRawBindingPort(context);
  const raw = new RawServiceMeshRuntime({
    descriptor: {
      meshName: 'liveness-disconnect-result',
      nodeRoutingId: rid,
      lifecycleGeneration: 1n,
      descriptorRevision: 1n,
      advertisedEndpoint: `inproc://liveness-result-${rid}-${process.pid}-${clock.id}`,
      channels: [],
      state: 'serving',
      securityIdentity: 'default',
      applicationVersion: 1n,
      protocolCapabilities: [M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
      objectRole: 'server',
      placementWeight: 1,
      activeCapacityLimit: 100,
      pendingCapacityLimit: 16,
      activeCapacityUsed: 0,
      pendingCapacityUsed: 0
    },
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: {
      createHost() {
        const host = port.createHost();
        return {
          createRouter() {
            router = host.createRouter();
            return router;
          },
          close: () => host.close(),
          shutdown: () => host.shutdown()
        };
      }
    }
  });
  raw.start();
  return { raw, router };
}

async function until(peers, predicate, label) {
  const deadline = Date.now() + 5000;
  while (!predicate() && Date.now() < deadline) {
    for (const value of peers) await value.raw.pumpBatch(true, true);
    await turn();
  }
  assert.ok(predicate(), label);
}

for (const terminated of [false, true]) {
  test(`liveness preserves native disconnect result and receive progress (terminated=${terminated})`, async (t) => {
    const clock = { now: 0, id: terminated ? 'error' : 'absent' };
    t.mock.method(performance, 'now', () => clock.now);
    const context = zlink.createContext();
    const source = peer('source', context, clock);
    const expired = peer('expired', context, clock);
    let healthy;
    try {
      source.raw.connectPeerByRoutingId(expired.router.localEndpoint(), 'expired');
      await until(
        [source, expired],
        () => source.raw.isPeerRouteReady('expired') && expired.raw.isPeerRouteReady('source'),
        'initial native handshake'
      );
      // No receive rotation runs between the physical removal and expiry.
      // Core owns the result; the retained logical admission still expires.
      if (terminated) {
        context.shutdown();
        assert.throws(
          () => source.raw.tickLiveness(DEFAULT_SERVICE_PEER_TIMEOUT_MS),
          (error) =>
            error instanceof zlink.ConnectError &&
            error.result === zlink.ConnectResult.InternalError
        );
      } else {
        source.router.disconnectRid('expired');
        assert.deepEqual(source.raw.tickLiveness(DEFAULT_SERVICE_PEER_TIMEOUT_MS).timedOutNodes, [
          'expired'
        ]);
        assert.equal(source.raw.isPeerRouteReady('expired'), false);
        clock.now = DEFAULT_SERVICE_PEER_TIMEOUT_MS;
        healthy = peer('healthy', context, clock);
        source.raw.connectPeerByRoutingId(healthy.router.localEndpoint(), 'healthy');
        await until(
          [source, healthy],
          () => source.raw.isPeerRouteReady('healthy') && healthy.raw.isPeerRouteReady('source'),
          'healthy native handshake after cleanup'
        );
        let received = 0;
        source.raw.setServiceIngress(() => {
          received++;
          return 'infrastructure';
        });
        healthy.raw.setServiceIngress(() => {
          received++;
          return 'infrastructure';
        });
        const payload = {
          packetName: 'LivenessAfterCleanup',
          contentType: 'application/json',
          payload: Buffer.from('{}')
        };
        assert.equal(await source.raw.sendToNode('healthy', payload), true);
        assert.equal(await healthy.raw.sendToNode('source', payload), true);
        await until(
          [source, healthy],
          () => received === 2,
          'both receive owners deliver subsequent sends'
        );
      }
    } finally {
      healthy?.raw.close();
      expired.raw.close();
      source.raw.close();
      context.close();
    }
  });
}
