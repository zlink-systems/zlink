'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const net = require('node:net');
const {
  ConnectionAttemptState,
  ConnectionFailureKind,
  ConnectionFailureStage
} = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  admitServiceRequest,
  requestDurableService
} = require('../../packages/framework/dist/runtime/foundation/durable-service-request');
const {
  RequestResult,
  SubmitResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const meshWire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const payload = {
  packetName: 'First',
  contentType: 'application/json',
  payload: Buffer.from('{"value":1}')
};
const turn = () => new Promise((resolve) => setImmediate(resolve));
const descriptor = (rid, endpoint = `inproc://${rid}`) => ({
  meshName: 'connection-admission',
  nodeRoutingId: rid,
  lifecycleGeneration: 7n,
  descriptorRevision: 1n,
  advertisedEndpoint: endpoint,
  channels: [],
  state: 'serving',
  securityIdentity: 'default',
  applicationVersion: 1n,
  protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
  objectRole: 'server',
  placementWeight: 100,
  activeCapacityLimit: 100,
  pendingCapacityLimit: 10,
  activeCapacityUsed: 0,
  pendingCapacityUsed: 0
});
function attempt(state = ConnectionAttemptState.Connecting, intentId = 1n, lastFailureId = 0n) {
  return {
    endpoint: 'inproc://target',
    state,
    intentId,
    lastFailureId,
    revision: 1n,
    lastFailure: {
      kind: ConnectionFailureKind.None,
      stage: ConnectionFailureStage.None,
      normalizedErrno: 0,
      disconnectReason: 0
    }
  };
}
function fixture(kind) {
  const target = descriptor('target');
  let rows = [];
  let snapshotReads = 0;
  const incoming = [],
    routes = new Map(),
    submissions = [];
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    bind() {},
    connectToRoutingId() {},
    connect() {},
    disconnect() {},
    disconnectRid() {},
    close() {},
    localEndpoint: () => 'inproc://source',
    receive: () => incoming.shift(),
    send: async () => {},
    submitSend: () => ({ result: SubmitResult.Ok, admitted: Promise.resolve() }),
    routesSnapshot: () =>
      [...routes].map(([routingId, routeGeneration]) => ({ routingId, routeGeneration })),
    connectionAttemptsSnapshot() {
      snapshotReads++;
      return rows;
    },
    request: (rid, parts, timeoutMs, onReply, onFailed, _received, onAdmitted) => {
      submissions.push({ rid, parts, timeoutMs });
      onAdmitted?.();
      const correlation = kind.startsWith('node')
        ? meshWire.decodeNodeRequestHeader(parts[0])
        : kind.startsWith('spot')
          ? wire.decodeStatefulHeader(parts[0]).correlation
          : 1n;
      return Promise.resolve(
        kind === 'lifecycle'
          ? [Buffer.from('reply')]
          : [
              kind.startsWith('node')
                ? meshWire.encodeReplyHeader(correlation, RequestResult.Ok, 0)
                : wire.encodeStatefulReply(correlation, RequestResult.Ok, 0),
              meshWire.encodeApplicationPayload(payload)
            ]
      ).then(onReply, onFailed);
    }
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, close() {} }) }
  });
  raw.start();
  raw.connectPeerByRoutingId(target.advertisedEndpoint, target.nodeRoutingId, 'default', 7n);
  const stateful = new ServiceStatefulRuntime(raw, 'source', 7n);
  const fence = {
    spot: { spotId: 'room', generation: 3n },
    targetNodeRid: 'target',
    targetNodeGeneration: 7n,
    authorityOwnerGeneration: 5n,
    ownerLeaseGeneration: 7n,
    storeVersion: 'version'
  };
  stateful.rememberSpotRoute(fence);
  return {
    raw,
    stateful,
    submissions,
    snapshotReads: () => snapshotReads,
    async snapshot(next) {
      rows = next;
      await raw.pumpBatch(false, false, true);
    },
    async admit() {
      routes.set('target', 9n);
      await raw.observeSelectedRoutes();
      incoming.push({
        sourceRid: 'target',
        routeGeneration: 9n,
        parts: [meshWire.encodeRouteMeshAdmission(meshWire.M6aServiceWireCommand.admit, target)],
        close() {}
      });
      assert.equal(await raw.pumpOne(), 'infrastructure');
    },
    start(timeoutMs = 1000) {
      if (kind === 'node') return raw.requestToNode('target', payload, timeoutMs).promise;
      if (kind === 'nodeObserved')
        return new Promise((resolve, reject) => {
          raw.requestToNode(
            'target',
            payload,
            timeoutMs,
            undefined,
            {
              operationCompleted: (_id, value) => resolve(value),
              operationFailed: (_id, error) => reject(error)
            },
            1
          );
        });
      if (kind === 'spotObserved')
        return new Promise((resolve, reject) => {
          const pending = stateful.requestToSpot(
            'source-room',
            fence,
            payload,
            timeoutMs,
            undefined,
            undefined,
            {
              operationCompleted: (_id, value) =>
                queueMicrotask(() => {
                  pending.complete(value);
                  resolve(value);
                }),
              operationFailed: (_id, error) =>
                queueMicrotask(() => {
                  pending.fail(error);
                  reject(error);
                })
            }
          );
          void pending.promise.catch(() => {});
        });
      if (kind === 'spot')
        return stateful.requestToSpot('source-room', fence, payload, timeoutMs).promise;
      const pending = raw.beginInfrastructureRequest(timeoutMs);
      void requestDurableService(raw, 'target', [Buffer.from('PREPARE')], pending, 'PREPARE').then(
        (value) => pending.complete(value),
        (error) => pending.fail(error)
      );
      return pending.promise;
    },
    close() {
      stateful.close();
      raw.close();
    }
  };
}
for (const kind of ['node', 'nodeObserved', 'spot', 'spotObserved', 'lifecycle']) {
  test(`${kind}: first remote request waits for connection and ADMIT, then submits once`, async () => {
    const f = fixture(kind);
    try {
      await f.snapshot([attempt()]);
      const pending = f.start();
      await turn();
      assert.equal(
        f.submissions.length,
        0,
        'unready connection must not receive a binding request'
      );
      await f.snapshot([attempt(ConnectionAttemptState.Connected)]);
      assert.equal(f.submissions.length, 0, 'Core connection alone is not service admission');
      await f.admit();
      await pending;
      assert.equal(f.submissions.length, 1);
      await f.snapshot([attempt(ConnectionAttemptState.Connected)]);
      await f.raw.observeSelectedRoutes();
      assert.equal(
        f.submissions.length,
        1,
        'completed binding operation must never be resubmitted'
      );
      assert.ok(f.submissions[0].timeoutMs <= 1000);
    } finally {
      f.close();
    }
  });
  test(`${kind}: failed endpoint returns Unavailable before the original deadline`, async () => {
    const f = fixture(kind);
    try {
      await f.snapshot([attempt()]);
      const started = performance.now();
      const rejected = assert.rejects(
        f.start(),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      await f.snapshot([attempt(ConnectionAttemptState.AttemptFailed, 1n, 2n)]);
      await rejected;
      assert.ok(performance.now() - started < 1000);
      assert.equal(f.submissions.length, 0);
    } finally {
      f.close();
    }
  });
  for (const scenario of ['recovered', 'replaced', 'removed', 'first-row-failed']) {
    test(`${kind}: waiting request rejects ${scenario} before submitting`, async () => {
      const f = fixture(kind);
      try {
        if (scenario !== 'first-row-failed') await f.snapshot([attempt()]);
        const rejected = assert.rejects(
          f.start(),
          (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
        );
        const next =
          scenario === 'removed'
            ? []
            : [
                attempt(
                  ConnectionAttemptState.Connected,
                  scenario === 'replaced' ? 3n : 1n,
                  scenario === 'recovered' || scenario === 'first-row-failed' ? 2n : 0n
                )
              ];
        await f.snapshot(next);
        await f.admit();
        await rejected;
        assert.equal(f.submissions.length, 0, 'recovery cannot revive an already waiting request');
      } finally {
        f.close();
      }
    });
  }
  test(`${kind}: already admitted route accepts a new request despite past outgoing failure`, async () => {
    const f = fixture(kind);
    try {
      await f.snapshot([attempt(ConnectionAttemptState.AttemptFailed, 1n, 2n)]);
      await f.admit();
      await f.start();
      assert.equal(f.submissions.length, 1);
    } finally {
      f.close();
    }
  });
  test(`${kind}: absent Core row waits and accepts its first successful intent`, async () => {
    const f = fixture(kind);
    try {
      const pending = f.start();
      await f.snapshot([attempt(ConnectionAttemptState.Connected, 3n)]);
      await f.admit();
      await pending;
      assert.equal(f.submissions.length, 1);
    } finally {
      f.close();
    }
  });
  test(`${kind}: readiness timeout uses the sender DeadlineExceeded`, async () => {
    const f = fixture(kind);
    try {
      const rejected = assert.rejects(
        f.start(1),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
      );
      await turn();
      const now = performance.now() + 10;
      f.raw.expireOperations(now, Number.POSITIVE_INFINITY);
      f.stateful.expireOperations(now, Number.POSITIVE_INFINITY);
      await rejected;
      assert.equal(f.submissions.length, 0);
    } finally {
      f.close();
    }
  });
  test(`${kind}: descriptor replacement invalidates the waiting request`, async () => {
    const f = fixture(kind);
    try {
      const rejected = assert.rejects(
        f.start(),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      f.raw.expectPeerByRoutingId('inproc://replacement', 'target', 'default', 8n);
      await rejected;
      assert.equal(f.submissions.length, 0);
    } finally {
      f.close();
    }
  });
  test(`${kind}: shutdown returns ShuttingDown without submitting`, async () => {
    const f = fixture(kind);
    try {
      const rejected = assert.rejects(
        f.start(),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.ShuttingDown
      );
      f.raw.close();
      await rejected;
      assert.equal(f.submissions.length, 0);
    } finally {
      f.close();
    }
  });
  test(`${kind}: preparation consumes the original sender timeout`, async (t) => {
    let now = 1000;
    t.mock.method(performance, 'now', () => now);
    const f = fixture(kind);
    try {
      const pending = f.start(1000);
      now += 40;
      await f.admit();
      await pending;
      assert.equal(f.submissions[0].timeoutMs, 960);
      assert.equal(f.submissions.length, 1);
    } finally {
      f.close();
    }
  });
  test(`${kind}: snapshot is consumed only on POLLCONNECT`, async () => {
    const f = fixture(kind);
    try {
      await f.raw.pumpBatch(false, false, false);
      await f.raw.pumpBatch(false, true, false);
      assert.equal(f.snapshotReads(), 0);
      await f.snapshot([attempt()]);
      assert.equal(f.snapshotReads(), 1);
    } finally {
      f.close();
    }
  });
}

for (const kind of ['node', 'nodeObserved', 'spot', 'spotObserved']) {
  test(`${kind}: ready submission allocates no admission listeners or attempt observation`, async (t) => {
    const f = fixture(kind);
    try {
      await f.admit();
      const unexpected = () => {
        throw new Error('Ready request must bypass waiting admission');
      };
      t.mock.method(f.raw, 'observePeerRoute', unexpected);
      t.mock.method(f.raw, 'observePeerRouteChanged', unexpected);
      t.mock.method(f.raw, 'observePeerConnectionIntentRemoved', unexpected);
      await f.start();
      assert.equal(f.submissions.length, 1);
    } finally {
      f.close();
    }
  });
}

test('ready admission creates no waiting subscriptions or attempt arrays', async (t) => {
  for (const ready of [false, true]) {
    const f = fixture('lifecycle');
    const costs = { route: 0, intent: 0, stop: 0, attempts: 0, promise: 0 };
    try {
      const rows = [attempt(ConnectionAttemptState.Connected)];
      await f.snapshot(rows);
      if (ready) await f.admit();
      for (const [method, counter] of [
        ['observePeerRouteChanged', 'route'],
        ['observePeerConnectionIntentRemoved', 'intent']
      ]) {
        const original = f.raw[method].bind(f.raw);
        t.mock.method(f.raw, method, (...args) => {
          costs[counter]++;
          return original(...args);
        });
      }
      const signal = f.raw.requestStopSignal;
      const add = signal.addEventListener.bind(signal);
      t.mock.method(signal, 'addEventListener', (...args) => {
        costs.stop++;
        return add(...args);
      });
      Object.defineProperty(rows, 'filter', {
        value: (...args) => {
          costs.attempts++;
          return Array.prototype.filter.apply(rows, args);
        }
      });
      const pending = f.raw.beginInfrastructureRequest(1000);
      const then = pending.promise.then.bind(pending.promise);
      const promiseProbe = t.mock.method(pending.promise, 'then', (...args) => {
        costs.promise++;
        return then(...args);
      });
      admitServiceRequest(
        f.raw,
        'target',
        pending,
        (remaining, finished) => {
          void f.raw.requestService('target', [Buffer.from('PREPARE')], remaining).then(
            (reply) => {
              finished();
              pending.complete(reply);
            },
            (error) => {
              finished();
              pending.fail(error);
            }
          );
        },
        (error) => pending.fail(error)
      );
      promiseProbe.mock.restore();
      if (!ready) await f.admit();
      await pending.promise;
      assert.equal(f.submissions.length, 1);
      t.diagnostic(JSON.stringify({ ready, costs }));
      if (ready) {
        assert.deepEqual(costs, { route: 0, intent: 0, stop: 0, attempts: 0, promise: 0 });
      } else {
        for (const [kind, count] of Object.entries(costs)) {
          assert.ok(count > 0, `waiting admission must exercise the ${kind} cost probe`);
        }
      }
    } finally {
      f.close();
    }
  }
});

test('ready observation borrows the socket snapshot without filtering or descriptor cloning', async (t) => {
  const f = fixture('node');
  try {
    const rows = [attempt(ConnectionAttemptState.Connected)];
    await f.snapshot(rows);
    await f.admit();
    Object.defineProperty(rows, 'filter', {
      value: () => {
        throw new Error('Ready observation must not allocate an attempt array');
      }
    });
    t.mock.method(f.raw.topology, 'peer', () => {
      throw new Error('Observation must use the owned peer, not a clone');
    });
    const observed = f.raw.observePeerRoute('target');
    assert.equal(observed.ready, true);
    assert.equal(observed.connectionAttempts, rows);
    assert.equal(f.raw.peerLifecycleGeneration('target'), 7n);
  } finally {
    f.close();
  }
});

test(
  'native refused endpoint completes first request as Unavailable without waiting for deadline',
  { timeout: 10000 },
  async () => {
    const {
      ZLinkNodeRawBindingPort
    } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
    const reservation = net.createServer();
    await new Promise((resolve) => reservation.listen(0, '127.0.0.1', resolve));
    const endpoint = `tcp://127.0.0.1:${reservation.address().port}`;
    await new Promise((resolve, reject) =>
      reservation.close((error) => (error ? reject(error) : resolve()))
    );
    const raw = new RawServiceMeshRuntime({
      descriptor: descriptor('native-source', 'tcp://127.0.0.1:0'),
      applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
      bindingPort: new ZLinkNodeRawBindingPort()
    });
    raw.start();
    let pending;
    let pumping = Promise.resolve();
    raw.setReadableHandler((receive, route, connect) => {
      pumping = pumping
        .then(() => raw.pumpBatch(receive, route, connect))
        .then(
          () => {},
          (error) => {
            pending?.fail(error);
          }
        );
    });
    try {
      raw.connectPeerByRoutingId(endpoint, 'dead-owner', 'default', 7n);
      const started = performance.now();
      pending = raw.requestToNode('dead-owner', payload, 5000);
      await assert.rejects(
        pending.promise,
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      assert.ok(performance.now() - started < 5000);
    } finally {
      await pumping;
      raw.close();
    }
  }
);

test(
  'native healthy connection preserves its intent while ADMIT waits and submits once',
  { timeout: 10000 },
  async (t) => {
    const {
      ZLinkNodeRawBindingPort
    } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
    let requesterRouter;
    const binding = new ZLinkNodeRawBindingPort();
    const createHost = binding.createHost.bind(binding);
    t.mock.method(binding, 'createHost', () => {
      const host = createHost();
      const createRouter = host.createRouter.bind(host);
      t.mock.method(host, 'createRouter', () => {
        requesterRouter = createRouter();
        return requesterRouter;
      });
      return host;
    });
    const make = (rid) =>
      new RawServiceMeshRuntime({
        descriptor: descriptor(rid, 'tcp://127.0.0.1:0'),
        applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
        bindingPort: binding
      });
    const owner = make('healthy-owner'),
      requester = make('healthy-requester');
    let ownerReleased = false,
      handled = 0,
      submitted = 0,
      pending;
    let ownerPump = Promise.resolve(),
      requesterPump = Promise.resolve();
    let connected;
    const connectionReady = new Promise((resolve) => {
      connected = resolve;
    });
    const snapshots = [];
    try {
      owner.start();
      requester.start();
      const router = requesterRouter;
      const snapshot = router.connectionAttemptsSnapshot.bind(router);
      t.mock.method(router, 'connectionAttemptsSnapshot', () => {
        const rows = snapshot();
        snapshots.push(rows);
        if (rows.some((row) => row.state === ConnectionAttemptState.Connected)) connected();
        return rows;
      });
      const request = router.request.bind(router);
      t.mock.method(router, 'request', (...args) => {
        submitted++;
        return request(...args);
      });
      owner.setServiceIngress((record) => {
        if (record.command !== meshWire.M6aServiceWireCommand.nodeRequest) return undefined;
        handled++;
        owner.replyService(record, [
          meshWire.encodeReplyHeader(meshWire.decodeNodeRequestHeader(record.parts[0])),
          meshWire.encodeApplicationPayload(payload)
        ]);
        return 'infrastructure';
      });
      owner.setReadableHandler((receive, route, connect) => {
        ownerPump = ownerPump.then(() => owner.pumpBatch(ownerReleased && receive, route, connect));
        void ownerPump.catch((error) => pending?.fail(error));
      });
      requester.setReadableHandler((receive, route, connect) => {
        requesterPump = requesterPump.then(() => requester.pumpBatch(receive, route, connect));
        void requesterPump.catch((error) => pending?.fail(error));
      });
      requester.connectPeer(
        owner.topology.localDescriptor().advertisedEndpoint,
        owner.topology.localDescriptor()
      );
      // Connect registration makes POLLCONNECT ready; consume it in the existing observer.
      await requester.pumpBatch(false, false, true);
      pending = requester.requestToNode('healthy-owner', payload, 5000);
      const result = pending.promise;
      void result.catch(() => {});
      await connectionReady;
      await requesterPump;
      assert.equal(submitted, 0, 'Core readiness cannot replace service ADMIT');
      const first = snapshots[0][0];
      assert.equal(first.state, ConnectionAttemptState.Connecting);
      assert.ok(
        snapshots.some((rows) => rows.some((row) => row.state === ConnectionAttemptState.Connected))
      );
      for (const rows of snapshots) {
        assert.equal(rows.length, 1, 'successful connection must retain its intent row');
        assert.equal(rows[0].intentId, first.intentId);
        assert.equal(rows[0].lastFailureId, 0n);
      }
      ownerReleased = true;
      ownerPump = ownerPump.then(() => owner.pumpBatch(true));
      await result;
      assert.equal(submitted, 1);
      assert.equal(handled, 1);
      t.diagnostic(
        JSON.stringify(snapshots, (_key, value) =>
          typeof value === 'bigint' ? value.toString() : value
        )
      );
    } finally {
      await Promise.allSettled([ownerPump, requesterPump]);
      requester.close();
      owner.close();
    }
  }
);
