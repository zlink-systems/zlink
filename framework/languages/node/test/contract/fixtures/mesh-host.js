const { ZLinkFrameworkRuntimeHost } = require('../../../packages/framework/dist/runtime/host');

// The test owns its injected MeshNode collaborator. Host fields remain private.
function createMeshHost(options, internalOptions) {
  const meshRuntime = { meshNode: () => undefined };
  const host = new ZLinkFrameworkRuntimeHost(options, {
    ...internalOptions,
    spotNodeRuntime: meshRuntime
  });
  return { host, meshRuntime };
}

module.exports = { createMeshHost };
