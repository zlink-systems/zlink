const { after } = require('node:test');
const { ZLinkSpotNodeRuntimeManager } = require('../../../packages/framework/dist/runtime/spots/spot-node-runtime-manager');
const { createFrameworkRegistration } = require('../../../packages/framework/dist/runtime/configuration');

// Startup owns publisher registration through the existing backend factory.
async function createPublisherRuntime(meshName, publisher, options = {}, nodeOverrides = {}) {
  const registration = options.registration ?? createFrameworkRegistration({});
  const mesh = registration.spotNodes.get(meshName);
  registration.spotNodes.set(meshName, { ...mesh, router: { bind: 'inproc://publisher-fixture', routingId: 'publisher-fixture', ...mesh?.router } });
  const node = {
    setCompletionHandler() {}, setBind() {}, configureObjectPlacement() {},
    start() {}, setReadyHandler() {},
    createReadyBatch: () => ({ reset() {}, close() {} }),
    createReceiveBatch: () => ({ reset() {}, close() {} }),
    drainReady: () => ({ ok: false, records: [] }),
    status: () => ({ localEndpoint: '' }),
    createPublisher: () => ({ close() {}, ...publisher }),
    async shutdown() {}, async close() {},
    ...nodeOverrides
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
