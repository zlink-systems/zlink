const path = require('node:path');

async function channelSendFixture(dist, backend = false) {
  const load = (relative) => require(path.join(dist, relative));
  const { SubmitResult } = load('runtime/backend/runtime-values');
  const wire = load('runtime/foundation/service-wire-m6a-codec');
  const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = load(
    'runtime/host/application-job-queue'
  );
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const received = [];
  let readable;
  let submit = () => ({ result: SubmitResult.Ok });
  const descriptor = (rid) => ({
    meshName: 'channel-send',
    nodeRoutingId: rid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://channel-send-${rid}`,
    channels: [{ name: 'channel', weight: 100 }],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'none',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 16,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  });
  const router = {
    setRoutingId() {},
    bind() {},
    close() {},
    setReceiveFlowState() {},
    localEndpoint: () => descriptor('local').advertisedEndpoint,
    setReadableHandler(handler) {
      readable = handler;
    },
    routesSnapshot: () => [],
    receive: () => received.shift(),
    submitSend: (...args) => submit(...args)
  };
  const bindingPort = {
    createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} })
  };
  let target;
  if (backend) {
    const { ZLinkNodeRawMeshBackend } = load('runtime/backend/node/node-raw-mesh-backend');
    target = new ZLinkNodeRawMeshBackend('channel-send', 'local', bindingPort, queue);
    target.setBind(descriptor('local').advertisedEndpoint);
  } else {
    const { RawServiceMeshRuntime } = load('runtime/foundation/raw-service-mesh-runtime');
    target = new RawServiceMeshRuntime({
      descriptor: descriptor('local'),
      bindingPort,
      applicationJobQueue: queue
    });
  }
  target.start();
  received.push({
    sourceRid: 'peer',
    sourceRoute: Buffer.from('peer'),
    routeGeneration: 1n,
    parts: [wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.hello, descriptor('peer'))],
    close() {}
  });
  if (backend) {
    readable(true, false);
    await new Promise(setImmediate);
  } else {
    await target.pumpOne(0);
  }
  return {
    target,
    setSubmit(value) {
      submit = value;
    },
    mockSubmit: (...args) => router.submitSend(...args),
    close: () => target.close()
  };
}

module.exports = { channelSendFixture };
