const assert = require('node:assert/strict');
const test = require('node:test');
const { MemoryRelocationStore } = require('./fixtures/instance-relocation-memory-store');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const integration = require('../../packages/framework/dist/nest-integration');
const { ZLinkNodeBackendAdapterFactory } = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-factory');
const { ReceiveKind } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

// The node quiescence owner is shared with Close. Cold activation preparation is
// a separate gate, so the queued case observes that owner without invoking Close.
async function settled(promise) {
  let done = false;
  void promise.then(() => { done = true; });
  await new Promise((resolve) => setImmediate(resolve));
  return done;
}

test('Close does not wait for the first terminal of an activated message that has not started', async () => {
  const captured = deferred();
  const release = deferred();
  class Ping {}
  class Room {}
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
  let intercepted = false;
  const backendAdapterFactory = new ZLinkNodeBackendAdapterFactory();
  const createMeshAdapter = backendAdapterFactory.createMeshAdapter.bind(backendAdapterFactory);
  backendAdapterFactory.createMeshAdapter = (...args) => {
    const adapter = createMeshAdapter(...args);
    const createMeshNode = adapter.createMeshNode.bind(adapter);
    adapter.createMeshNode = (...nodeArgs) => {
      node = createMeshNode(...nodeArgs);
      const createReadyBatch = node.createReadyBatch.bind(node);
      node.createReadyBatch = (...batchArgs) => {
        const batch = createReadyBatch(...batchArgs);
        const takeClaim = batch.takeClaim.bind(batch);
        batch.takeClaim = (...claimArgs) => {
          const claim = takeClaim(...claimArgs);
          const recvBatch = claim.recvBatch.bind(claim);
          claim.recvBatch = (...receiveArgs) => {
            const result = recvBatch(...receiveArgs);
            return {
              ...result,
              records: result.records.map((record) => {
                if (intercepted || record.kind !== ReceiveKind.InstanceSpotActivation ||
                    record.activationRecord?.activation !== 'missing' ||
                    record.activationRecord.target.targetSpotId !== 'room-1') return record;
                intercepted = true;
                assert.equal(typeof record.onHandlerTurnStarted, 'function');
                assert.equal(typeof record.onTerminalCompletion, 'function');
                // Inherit the real getters, callback identities, parts and permit.
                // Only preparation changes; its original receiver remains intact.
                return Object.create(record, {
                  prepareDispatch: {
                    value: async (signal) => {
                      captured.resolve(record);
                      await release.promise;
                      return record.prepareDispatch === undefined
                        ? true
                        : record.prepareDispatch.call(record, signal);
                    }
                  }
                });
              })
            };
          };
          return claim;
        };
        return batch;
      };
      return node;
    };
    return adapter;
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration }, { backendAdapterFactory });
  integration.createIntegrationSpotManager(registration, host);
  const outbound = integration.createIntegrationSpotOutbound(host);
  let completion;
  const errors = [];
  try {
    await host.start();
    const request = outbound.requestToSpot('room-1', new Ping()).instanceSpot('Room').submit();
    let requestSettled = false;
    void request.then(() => { requestSettled = true; }, () => { requestSettled = true; });
    // Missing packet registration fails before handler entry. The existing reply
    // mapper exposes a configuration error as a typed InternalFailure.
    completion = assert.rejects(request, (error) => {
      assert.ok(error instanceof framework.ZLinkFrameworkException);
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
      assert.equal(error.message, 'SPOT route handler not found: Ping');
      return true;
    });
    void completion.catch(() => {});
    await Promise.race([
      captured.promise,
      completion.then(() => { throw new Error('Instance request completed before activation capture'); })
    ]);
    assert.equal(typeof node.waitForInstanceApplicationQuiescence, 'function');
    const wait = node.waitForInstanceApplicationQuiescence('room-1');
    assert.equal(await settled(wait), true);
    assert.equal(requestSettled, false);
    release.resolve();
    await completion;
    assert.equal(await settled(wait), true);
  } catch (error) {
    errors.push(error);
  } finally {
    release.resolve();
    try {
      if (completion) await completion;
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

function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
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
