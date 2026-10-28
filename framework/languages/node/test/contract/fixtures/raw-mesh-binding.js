// The binding owns the transport instance; tests retain that dependency rather
// than replacing the runtime's private router after construction.
function rawMeshBinding(overrides = {}) {
  const records = [];
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    setReadableHandler() {},
    bind() {},
    localEndpoint: () => 'inproc://raw-mesh-test',
    routesSnapshot: () => [],
    receive: () => records.shift(),
    close() {},
    ...overrides
  };
  const bindingPort = {
    createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} })
  };
  return { bindingPort, router, records };
}


function serviceNodeDescriptor(nodeRoutingId, overrides = {}) {
  const { SERVICE_WIRE_REQUIRED_CAPABILITY } = require('../../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
  return {
    meshName: 'mesh', nodeRoutingId, lifecycleGeneration: 1n, descriptorRevision: 1n,
    advertisedEndpoint: `inproc://${nodeRoutingId}`, channels: [], state: 'serving',
    securityIdentity: 'test', applicationVersion: 0n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
    placementWeight: 1, activeCapacityLimit: 10, pendingCapacityLimit: 10,
    activeCapacityUsed: 0, pendingCapacityUsed: 0, ...overrides
  };
}

function rawMeshFixture(options = {}, overrides = {}) {
  const { RawServiceMeshRuntime } = require('../../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
  const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../../packages/framework/dist/runtime/host/application-job-queue');
  const binding = rawMeshBinding(overrides);
  const runtime = new RawServiceMeshRuntime({
    descriptor: serviceNodeDescriptor('source'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    ...options,
    bindingPort: binding.bindingPort
  });
  runtime.start();
  return { ...binding, runtime };
}

module.exports = { rawMeshBinding, rawMeshFixture, serviceNodeDescriptor };
