const {
  ZLinkRouteMeshRuntimeCoordinator
} = require('../../../packages/framework/dist/runtime/host/route-mesh-runtime');
const { ZLinkRuntimeAdmissionGate } = require('../../../packages/framework/dist/runtime/admission');
const { ZLinkFrameworkRuntimeHost } = require('../../../packages/framework/dist/runtime/host');
const {
  ZLinkBoundSessionRelay
} = require('../../../packages/framework/dist/runtime/host/bound-session-relay');

function createRelayHost(options, internalOptions) {
  /** @type {import('../../../packages/framework/dist/runtime/host/bound-session-relay').ZLinkBoundSessionRelay} */
  let relay;
  /** @type {import('../../../packages/framework/dist/runtime/host/route-mesh-runtime').ZLinkRouteMeshRuntimeCoordinator} */
  let coordinator;
  /** @type {import('../../../packages/framework/dist/runtime/admission').ZLinkRuntimeAdmissionGate} */
  let admission;
  const host = new ZLinkFrameworkRuntimeHost(options, {
    ...internalOptions,
    createAdmissionGate(ownerOpen) {
      admission = new ZLinkRuntimeAdmissionGate(ownerOpen);
      return admission;
    },
    createRouteMeshCoordinator(meshOptions) {
      coordinator =
        internalOptions?.createRouteMeshCoordinator?.(meshOptions) ??
        new ZLinkRouteMeshRuntimeCoordinator(meshOptions);
      return coordinator;
    },
    createBoundSessionRelay(relayOptions) {
      relay = new ZLinkBoundSessionRelay(relayOptions);
      return relay;
    }
  });
  return { host, relay, coordinator, admission };
}

module.exports = { createRelayHost };
