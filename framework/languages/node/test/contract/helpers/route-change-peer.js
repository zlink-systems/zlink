const assert = require('node:assert/strict');
const net = require('node:net');
const { setImmediate: turn } = require('node:timers/promises');

async function routeChangePeers({
  sourceRid = 'a-source',
  targetRid = 'z-target',
  sourceGeneration = 1n,
  targetGeneration = 1n,
  runtimeRoot = require('node:path').resolve(__dirname, '../../../packages/framework/dist/runtime')
} = {}) {
  const { RawServiceMeshRuntime } = require(runtimeRoot + '/foundation/raw-service-mesh-runtime');
  const { ZLinkNodeRawBindingPort } = require(runtimeRoot + '/backend/node/node-raw-binding-port');
  const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require(
    runtimeRoot + '/host/application-job-queue'
  );
  const { M6A_SERVICE_WIRE_REQUIRED_CAPABILITY } = require(
    runtimeRoot + '/foundation/service-wire-m6a-codec'
  );
  const sockets = new Set();
  const held = [];
  let holding = false;
  let heldBytes = 0;
  let destination;
  const server = net.createServer((client) => {
    const address = new URL(destination);
    const upstream = net.connect(Number(address.port), address.hostname);
    for (const socket of [client, upstream]) {
      sockets.add(socket);
      socket.on('close', () => sockets.delete(socket));
      socket.on('error', (error) => {
        if (error.code !== 'ECONNRESET') throw error;
      });
    }
    client.pipe(upstream);
    upstream.on('data', (bytes) => {
      if (holding) {
        held.push([client, bytes]);
        heldBytes += bytes.length;
      } else client.write(bytes);
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const proxyEndpoint = `tcp://127.0.0.1:${server.address().port}`;
  function peer(rid, advertisedEndpoint, lifecycleGeneration) {
    let router;
    const port = new ZLinkNodeRawBindingPort();
    const raw = new RawServiceMeshRuntime({
      descriptor: {
        meshName: 'route-change',
        nodeRoutingId: rid,
        lifecycleGeneration,
        descriptorRevision: 1n,
        advertisedEndpoint: 'tcp://127.0.0.1:0',
        channels: [],
        state: 'serving',
        securityIdentity: 'default',
        applicationVersion: 1n,
        protocolCapabilities: [M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
        objectRole: 'server',
        placementWeight: 100,
        activeCapacityLimit: 100,
        pendingCapacityLimit: 16,
        activeCapacityUsed: 0,
        pendingCapacityUsed: 0
      },
      resolveAdvertisedEndpoint: advertisedEndpoint,
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
    return { raw, router, descriptor: raw.topology.localDescriptor() };
  }
  const source = peer(sourceRid, () => proxyEndpoint, sourceGeneration);
  destination = source.router.localEndpoint();
  const target = peer(targetRid, undefined, targetGeneration);
  const events = [];
  let targetRequests = 0;
  const receive = target.router.receive.bind(target.router);
  target.router.receive = (...args) => {
    const record = receive(...args);
    if (record?.requestSeq !== undefined) targetRequests++;
    return record;
  };
  async function until(predicate, label, deadline = performance.now() + 1000) {
    while (!predicate() && performance.now() < deadline) {
      await source.raw.pumpBatch(true, true);
      await target.raw.pumpBatch(true, true);
      await turn();
    }
    const snapshot = {
      label,
      source: source.raw.observePeerRoute(targetRid),
      target: target.raw.observePeerRoute(sourceRid)
    };
    events.push(snapshot);
    assert(
      predicate(),
      JSON.stringify(snapshot, (_, value) => (typeof value === 'bigint' ? String(value) : value))
    );
  }
  source.raw.expectPeerByRoutingId(target.descriptor.advertisedEndpoint, targetRid);
  target.raw.connectPeerByRoutingId(proxyEndpoint, sourceRid);
  return {
    source,
    target,
    until,
    events,
    targetRequests: () => targetRequests,
    async untilTargetOnly(predicate, label) {
      const deadline = performance.now() + 1000;
      while (!predicate() && performance.now() < deadline) {
        await target.raw.pumpBatch(true, true);
        await turn();
      }
      assert(predicate(), label);
    },
    async replaceBeforeSourceObservation() {
      const before = [
        source.raw.observePeerRoute(targetRid).generation,
        target.raw.observePeerRoute(sourceRid).generation
      ];
      source.raw.connectPeerByRoutingId(target.descriptor.advertisedEndpoint, targetRid);
      const changed = () =>
        source.router
          .routesSnapshot()
          .some((route) => route.routingId === targetRid && route.routeGeneration !== before[0]) &&
        target.router
          .routesSnapshot()
          .some((route) => route.routingId === sourceRid && route.routeGeneration !== before[1]);
      await this.untilTargetOnly(changed, 'Core selects replacement before source observes it');
      await target.raw.observeSelectedRoutes();
    },
    hold() {
      holding = true;
    },
    held: () => heldBytes > 0,
    release() {
      holding = false;
      for (const [client, bytes] of held.splice(0)) client.write(bytes);
    },
    async switchRoute() {
      const before = [
        source.raw.observePeerRoute(targetRid).generation,
        target.raw.observePeerRoute(sourceRid).generation
      ];
      source.raw.connectPeerByRoutingId(target.descriptor.advertisedEndpoint, targetRid);
      await until(
        () =>
          source.raw.observePeerRoute(targetRid).generation !== before[0] &&
          target.raw.observePeerRoute(sourceRid).generation !== before[1],
        'both Core selections change'
      );
    },
    async close() {
      source.raw.close();
      target.raw.close();
      for (const socket of sockets) socket.destroy();
      await new Promise((resolve) => server.close(resolve));
    }
  };
}
module.exports = { routeChangePeers };
