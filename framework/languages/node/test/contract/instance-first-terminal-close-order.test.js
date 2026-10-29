const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const integration = require('../../packages/framework/dist/nest-integration');
const { ZLinkNodeBackendAdapterFactory } = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-factory');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');

// Spot messaging §4 step 12 with §7 step 1: a Close waits for the first terminal
// record of an activated message whose handler turn started before it. A message
// still queued behind that Close has not started, so the Close must not wait for
// it; otherwise the Close and the queued message wait for each other.
function runtime() {
  return new ServiceStatefulRuntime({
    topology: { peer: () => ({ descriptor: { lifecycleGeneration: 5n } }) },
    observePeerRoute: () => ({ generation: 1n, ready: true }),
    observePeerRouteChanged: () => () => {},
    observePeerConnectionIntentRemoved: () => () => {},
    setServiceIngress() {},
    replyService() {}
  }, 'target', 7n);
}

const target = { targetSpotId: 'room-1', stableType: 'Room' };
const route = {
  targetSpotId: 'room-1',
  targetNodeRid: 'target',
  targetNodeGeneration: 7n,
  objectGeneration: 1n,
  authorityOwnerGeneration: 1n,
  ownerId: 'owner',
  leaseGeneration: 1n,
  storeVersion: 'v1'
};

async function settled(promise) {
  let done = false;
  void promise.then(() => { done = true; });
  await new Promise((resolve) => setImmediate(resolve));
  return done;
}

test('Close does not wait for the first terminal of an activated message that has not started', async () => {
  const stateful = runtime();
  try {
    const queued = stateful.activationTerminalCompletion(target, route);
    assert.equal(await settled(stateful.waitForInstanceApplicationQuiescence('room-1')), true);
    await queued.onTerminalCompletion();
    assert.equal(await settled(stateful.waitForInstanceApplicationQuiescence('room-1')), true);
  } finally {
    stateful.close();
  }
});

function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
}

class MemoryRelocationStore {
  blobs = new Map();
  async read(reference, signal) {
    signal?.throwIfAborted();
    const storeNow = new Date();
    const blob = this.blobs.get(reference.value);
    if (!blob || blob.expiresAt <= storeNow) return { kind: 'missing', storeNow };
    return { kind: 'found', ...blob, bytes: Buffer.from(blob.bytes), storeNow };
  }
  async put(reference, payload, retentionMs, signal) {
    const current = await this.read(reference, signal);
    const storeNow = new Date();
    if (current.kind === 'found')
      return Buffer.from(payload).equals(current.bytes)
        ? { kind: 'alreadyStored', expiresAt: current.expiresAt, storeNow }
        : { kind: 'conflict', storeNow };
    const expiresAt = new Date(storeNow.getTime() + retentionMs);
    this.blobs.set(reference.value, { bytes: Buffer.from(payload), expiresAt });
    return { kind: 'stored', expiresAt, storeNow };
  }
  async renew(reference, retentionMs, signal) {
    const current = await this.read(reference, signal);
    if (current.kind === 'missing') return current;
    const storeNow = new Date();
    const expiresAt = new Date(storeNow.getTime() + retentionMs);
    this.blobs.set(reference.value, { bytes: current.bytes, expiresAt });
    return { kind: 'renewed', expiresAt, storeNow };
  }
  async delete(reference, signal) {
    signal?.throwIfAborted();
    this.blobs.delete(reference.value);
  }
}

test('Close waits for the first terminal record of a started activated message', async () => {
  const entered = deferred();
  const release = deferred();
  class Ping {}
  const PingHandler = class Ping {
    async handle() {
      entered.resolve();
      await release.promise;
      return { ok: true };
    }
  };
  class Room {
    onInitialize() {
      this.context.handlers.addPacket(PingHandler);
    }
  }
  const builder = nestjs.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  builder.addRelocationStore(new MemoryRelocationStore());
  builder
    .addRouteMesh('first-terminal')
    .listen('tcp://127.0.0.1:0')
    .routingId('first-terminal-node')
    .setAdvertiseHost('127.0.0.1')
    .objects()
    .server()
    .addInstanceSpotFactory('Room', Room, (factory) => factory.disableRelocation());
  const built = builder.build();
  const registration = framework.createFrameworkRegistration({
    locations: built.locations,
    spotNodes: built.spotNodes
  });
  let node;
  const backendAdapterFactory = new ZLinkNodeBackendAdapterFactory();
  const createMeshAdapter = backendAdapterFactory.createMeshAdapter.bind(backendAdapterFactory);
  backendAdapterFactory.createMeshAdapter = (...args) => {
    const adapter = createMeshAdapter(...args);
    const createMeshNode = adapter.createMeshNode.bind(adapter);
    adapter.createMeshNode = (...nodeArgs) => {
      node = createMeshNode(...nodeArgs);
      return node;
    };
    return adapter;
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration }, { backendAdapterFactory });
  integration.createIntegrationSpotManager(registration, host);
  const outbound = integration.createIntegrationSpotOutbound(host);
  let request;
  const errors = [];
  try {
    await host.start();
    request = outbound.requestToSpot('room-1', new Ping()).instanceSpot('Room').submit();
    void request.catch(() => {});
    await Promise.race([
      entered.promise,
      request.then(() => { throw new Error('Instance request completed before handler entry'); })
    ]);
    assert.equal(typeof node.waitForInstanceApplicationQuiescence, 'function');
    const wait = node.waitForInstanceApplicationQuiescence('room-1');
    assert.equal(await settled(wait), false);
    release.resolve();
    await request;
    assert.equal(await settled(wait), true);
  } catch (error) {
    errors.push(error);
  } finally {
    release.resolve();
    try {
      if (request) await request;
    } catch (error) {
      if (!errors.includes(error)) errors.push(error);
    } finally {
      try {
        await host.stop();
      } catch (error) {
        errors.push(error);
      }
    }
  }
  if (errors.length === 1) throw errors[0];
  if (errors.length > 1) throw new AggregateError(errors, 'Instance terminal assertion or cleanup failed');
});
