const { after } = require('node:test');
const { ZLinkSpotNodeRuntimeManager } = require('../../../packages/framework/dist/runtime/spots/spot-node-runtime-manager');
const { createFrameworkRegistration } = require('../../../packages/framework/dist/runtime/configuration');

// Startup owns publisher registration through the existing backend factory.
async function createPublisherRuntime(meshName, publisher, options = {}) {
  const registration = createFrameworkRegistration({});
  registration.spotNodes.set(meshName, { router: { bind: 'inproc://publisher-fixture', routingId: 'publisher-fixture' } });
  const node = {
    setCompletionHandler() {}, setBind() {}, configureObjectPlacement() {},
    start() {}, setReadyHandler() {},
    createReadyBatch: () => ({ reset() {}, close() {} }),
    createReceiveBatch: () => ({ reset() {}, close() {} }),
    drainReady: () => ({ ok: false, records: [] }),
    status: () => ({ localEndpoint: '' }),
    createPublisher: () => ({ close() {}, ...publisher }),
    async shutdown() {}, async close() {}
  };
  const runtime = new ZLinkSpotNodeRuntimeManager({
    ...options, registration,
    backendAdapterFactory: { createMeshAdapter: () => ({ createMeshNode: () => node }) }
  });
  await runtime.start();
  after(() => runtime.dispose());
  return runtime;
}
module.exports = { createPublisherRuntime };
