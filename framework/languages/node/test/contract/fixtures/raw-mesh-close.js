const {
  ZLinkNodeRawMeshBackend
} = require('../../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { SubmitResult } = require('../../../packages/framework/dist/runtime/backend/runtime-values');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../../packages/framework/dist/runtime/host/application-job-queue');

function deferred() {
  let resolve;
  const promise = new Promise((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

function fixture(taskRunner) {
  const events = [];
  let closed = false;
  let routes = [];
  let readable;
  const f = { events, onSubmit() {} };
  const guard = (method) => {
    if (closed) throw new Error(`router.${method} after close`);
  };
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    setReadableHandler(handler) {
      readable = handler;
    },
    bind() {},
    localEndpoint: () => 'tcp://127.0.0.2:28740',
    routesSnapshot() {
      guard('routesSnapshot');
      return routes;
    },
    submitSend(target) {
      guard('submitSend');
      events.push(`submit:${target}`);
      return f.onSubmit(target) ?? { result: SubmitResult.Ok, admitted: Promise.resolve() };
    },
    receive() {
      guard('receive');
      return undefined;
    }
  };
  const backend = new ZLinkNodeRawMeshBackend(
    'close-mesh',
    'local',
    {
      createHost: () => ({
        createRouter: () => router,
        shutdown() {},
        close() {
          events.push('host.close');
          closed = true;
        }
      })
    },
    new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2048n })
    ),
    undefined,
    undefined,
    taskRunner
  );
  backend.setBind('tcp://127.0.0.2:0');
  backend.start();
  Object.assign(f, {
    backend,
    runtime: backend.runtime,
    readable: (...args) => readable(...args),
    setRoutes(next) {
      routes = next;
    },
    closed: () => closed
  });
  return f;
}

async function closeDuringHello(onHelloSubmitted) {
  const f = fixture();
  const stopped = deferred();
  let first = true;
  f.onSubmit = (target) => {
    onHelloSubmitted?.(target);
    if (first) {
      first = false;
      queueMicrotask(() => {
        f.events.push('close:start');
        Promise.resolve(f.backend.close()).then(() => {
          f.events.push('close:complete');
          stopped.resolve();
        });
      });
    }
  };
  f.setRoutes([
    { routingId: 'peer-a', routeGeneration: 1n },
    { routingId: 'peer-b', routeGeneration: 1n }
  ]);
  f.readable(false, true);
  await stopped.promise;
  return f.events;
}

module.exports = { deferred, fixture, closeDuringHello };

if (require.main === module) {
  closeDuringHello().then((events) => {
    for (const event of events) console.log(event);
  });
}
