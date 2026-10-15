const assert = require('node:assert/strict');
const test = require('node:test');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  RequestResult,
  SubmitResult,
  ZLinkBackendResultError
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const zlink = require('@zlink-systems/zlink');
const {
  ZLinkNodeRawBindingPort
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const { setImmediate: turn } = require('node:timers/promises');

//  Core ROUTER §10.1 selects one route per RID; the Framework observes that
//  selection through its snapshot (transport liveness §5).

function descriptor(nodeRoutingId) {
  return {
    meshName: 'selected-route-mesh',
    nodeRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://selected-route-${nodeRoutingId}`,
    channels: [{ name: 'alpha', weight: 100 }],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 16,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

function selectedRouteRuntime(routes) {
  const sent = [];
  const requested = [];
  const disconnected = [];
  const admissionsEnded = [];
  let monitorOpened = false;
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    setReadableHandler() {},
    bind() {},
    localEndpoint: () => 'inproc://selected-route-local',
    connect() {},
    connectToRoutingId() {},
    disconnect() {},
    disconnectRid(rid) {
      disconnected.push(rid);
      routes.delete(rid);
    },
    monitor() {
      monitorOpened = true;
      return { drain: () => 0, statusReady: () => true, close() {} };
    },
    routesSnapshot: () =>
      [...routes].map(([routingId, routeGeneration]) => ({ routingId, routeGeneration })),
    submitSend(target, parts) {
      sent.push([target, parts[0][3]]);
      return { result: SubmitResult.Ok, admitted: Promise.resolve() };
    },
    async send(target, parts) {
      sent.push([target, parts[0][3]]);
    },
    async request(target) {
      //  Core decides whether the selected route carries the request.
      requested.push(target);
      throw new ZLinkBackendResultError('request', RequestResult.Busy);
    },
    receive: () => undefined,
    close() {}
  };
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local'),
    onPeerDisconnected: (rid) => admissionsEnded.push(rid),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  runtime.start();
  return {
    runtime,
    sent,
    requested,
    disconnected,
    admissionsEnded,
    monitorOpened: () => monitorOpened,
    processReceived: (record) =>
      runtime.processReceived(
        { ...record, sourceRoute: Buffer.from(record.sourceRid), close() {} },
        performance.now(),
        undefined
      )
  };
}

test('RouteMesh admission follows the observed Core selected route (FW01)', async () => {
  const peer = descriptor('peer');
  const routes = new Map([['peer', 11n]]);
  const harness = selectedRouteRuntime(routes);
  const { runtime, sent, processReceived } = harness;
  const hello = (routeGeneration) =>
    processReceived({
      sourceRid: 'peer',
      routeGeneration,
      parts: [Buffer.from(wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.hello, peer))]
    });
  try {
    runtime.connectPeerByRoutingId(peer.advertisedEndpoint, 'peer');
    await runtime.pumpBatch(false, true);
    for (let round = 0; round < 3; round++) await runtime.pumpBatch(false, true);
    assert.equal(
      sent.filter(
        ([target, command]) => target === 'peer' && command === wire.M6aServiceWireCommand.hello
      ).length,
      1
    );

    // Core delivers the handshake of the selected route.
    assert.equal(await hello(11n), 'infrastructure');
    assert.equal(runtime.topology.peer('peer')?.connectionId, 'route:11');
    for (let round = 0; round < 3; round++) await runtime.pumpBatch(false, true);
    assert.equal(
      sent.filter(
        ([target, command]) => target === 'peer' && command === wire.M6aServiceWireCommand.hello
      ).length,
      1
    );

    // Core replaces the route: the admission ends and the new route handshakes.
    routes.set('peer', 12n);
    sent.length = 0;
    await runtime.pumpBatch(false, true);
    assert.equal(runtime.topology.peer('peer'), undefined);
    assert.ok(
      sent.some(
        ([target, command]) => target === 'peer' && command === wire.M6aServiceWireCommand.hello
      )
    );
    assert.equal(await hello(12n), 'infrastructure');
    assert.equal(runtime.topology.peer('peer')?.connectionId, 'route:12');
    for (let round = 0; round < 3; round++) await runtime.pumpBatch(false, true);
    assert.equal(
      sent.filter(
        ([target, command]) => target === 'peer' && command === wire.M6aServiceWireCommand.hello
      ).length,
      1
    );

    // The route disappears: nothing stays admitted on it.
    routes.delete('peer');
    await runtime.pumpBatch(false, true);
    assert.equal(runtime.topology.peer('peer'), undefined);
    assert.equal(runtime.isPeerRouteReady('peer'), false);

    // Route state never came from a ROUTER monitor.
    assert.equal(harness.monitorOpened(), false);
  } finally {
    runtime.close();
  }
});

test('liveness expiry ends admission and removes the physical target once', () => {
  const h = selectedRouteRuntime(new Map([['peer', 11n]]));
  try {
    assert.equal(h.runtime.admitPeer(descriptor('peer'), 11n, 0), 'admitted');
    assert.deepEqual(h.runtime.tickLiveness(15000).timedOutNodes, ['peer']);
    assert.equal(h.runtime.topology.peer('peer'), undefined);
    assert.deepEqual(h.admissionsEnded, ['peer']);
    assert.deepEqual(h.disconnected, ['peer']);
    h.runtime.tickLiveness(15001);
    assert.deepEqual(h.admissionsEnded, ['peer']);
    assert.deepEqual(h.disconnected, ['peer']);
  } finally {
    h.runtime.close();
  }
});

test('current receive refreshes liveness without removing the target', () => {
  const h = selectedRouteRuntime(new Map([['peer', 11n]]));
  try {
    h.runtime.admitPeer(descriptor('peer'), 11n, 0);
    h.runtime.topology.peer('peer').liveness.recordReceived(14000);
    assert.deepEqual(h.runtime.tickLiveness(15000).timedOutNodes, []);
    assert.ok(h.runtime.topology.peer('peer'));
    assert.deepEqual(h.disconnected, []);
    assert.deepEqual(h.admissionsEnded, []);
  } finally {
    h.runtime.close();
  }
});

test('expiry notification can admit a replacement without closing its target', () => {
  const routes = new Map([['peer', 11n]]);
  const h = selectedRouteRuntime(routes);
  try {
    h.runtime.admitPeer(descriptor('peer'), 11n, 0);
    h.runtime.onPeerDisconnected = () => {
      routes.set('peer', 12n);
      h.runtime.admitPeer(descriptor('peer'), 12n, 14000);
    };
    h.runtime.tickLiveness(15000);
    assert.equal(routes.get('peer'), 12n);
    assert.equal(h.runtime.topology.peer('peer').connectionId, 'route:12');
    assert.deepEqual(h.disconnected, ['peer']);
  } finally {
    h.runtime.close();
  }
});

test('native target removal settles pending admission once on liveness expiry', async () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  let admissionEnds = 0;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor('local'),
    applicationJobQueue: queue,
    onPeerDisconnected: () => admissionEnds++,
    bindingPort: new ZLinkNodeRawBindingPort(context)
  });
  const target = zlink.createDealerSocket(context);
  let monitor;
  try {
    runtime.start();
    const router = runtime.router;
    const recordBytes = 65536;
    const recordHwm = BigInt(recordBytes + 64);
    router.socket.options.sendHwm = recordHwm;
    target.setRoutingId(zlink.RoutingId.from('peer'));
    target.options.recvHwm = recordHwm;
    target.setReceiveFlowState(zlink.ReceiveFlowState.PAUSED);
    monitor = router.monitor();
    target.connect(router.localEndpoint());
    const selectedBy = performance.now() + 2000;
    let route;
    while (!route && performance.now() < selectedBy) {
      route = router.routesSnapshot().find((value) => value.routingId === 'peer');
      await turn();
    }
    assert.ok(route);
    assert.equal(runtime.admitPeer(descriptor('peer'), route.routeGeneration, 0), 'admitted');
    const pending = [];
    let terminal;
    let terminalCount = 0;
    for (let record = 0; record < 16; record++) {
      const submission = router.submitSend('peer', [Buffer.alloc(recordBytes)]);
      // Observe every filler, including faults from the same target removal.
      pending.push(
        submission.admitted.then(
          () => 'admitted',
          (error) => error
        )
      );
    }
    pending.at(-1).then((value) => {
      terminal = value;
      terminalCount++;
    });
    await turn();
    assert.equal(terminalCount, 0, 'fixture must retain a native admission');
    runtime.tickLiveness(15000);
    runtime.tickLiveness(15001);
    const settledBy = performance.now() + 2000;
    while (terminalCount === 0 && performance.now() < settledBy) await turn();
    assert.equal(terminalCount, 1);
    assert.ok(terminal instanceof ZLinkBackendResultError);
    assert.equal(terminal.result, SubmitResult.NotFound);
    assert.equal(admissionEnds, 1);
    let physicalEnds = 0;
    while (physicalEnds === 0 && performance.now() < settledBy) {
      monitor.drain((event) => {
        if (event.event === zlink.MonitorEventType.Disconnected) physicalEnds++;
      });
      await turn();
    }
    assert.equal(physicalEnds, 1);
    await Promise.all(pending);
  } finally {
    monitor?.close();
    target.close();
    runtime.close();
    context.close();
  }
});

test('a retired liveness receipt cannot expire the replacement admission', () => {
  const h = selectedRouteRuntime(new Map([['peer', 12n]]));
  try {
    h.runtime.admitPeer(descriptor('peer'), 11n, 0);
    const retired = h.runtime.topology.peer('peer').liveness;
    h.runtime.admitPeer(descriptor('peer'), 12n, 14000);
    assert.equal(retired.isExpired(15000), true);
    assert.deepEqual(h.runtime.tickLiveness(15000).timedOutNodes, []);
    assert.equal(h.runtime.topology.peer('peer').connectionId, 'route:12');
    assert.deepEqual(h.disconnected, []);
  } finally {
    h.runtime.close();
  }
});

test('a direct RID request selects only the logical target and leaves route acceptance to Core (FW02)', async () => {
  const peer = descriptor('peer');
  const { runtime, requested } = selectedRouteRuntime(new Map([['peer', 21n]]));
  const payload = {
    packetName: 'DirectQuestion',
    contentType: 'application/json',
    payload: Buffer.from('question')
  };
  try {
    // An RID that is not an admitted peer is no target: the requester's selection.
    assert.deepEqual(await runtime.requestToNode('unknown', payload, 1_000).promise, {
      terminalResult: RequestResult.NotFound,
      failureCode: 14
    });
    assert.equal(runtime.topology.admit(peer, 'route:20'), 'admitted');
    assert.equal(runtime.topology.disconnect('peer', 'route:20'), true);
    assert.notEqual(runtime.topology.knownDescriptor('peer'), undefined);
    assert.deepEqual(await runtime.requestToNode('peer', payload, 1_000).promise, {
      terminalResult: RequestResult.NotFound,
      failureCode: 14
    });
    assert.deepEqual(requested, []);

    // An admitted target is submitted to Core even before its first liveness
    // ACK; Core's REQUEST result is the terminal, not a Framework copy.
    assert.equal(runtime.topology.admit(peer, 'route:21'), 'admitted');
    runtime.liveness.admit('peer', 'route:21', performance.now());
    // 05-transport-liveness.ko.md:228 (§5): handshake readiness does not wait for ACK.
    assert.equal(runtime.isPeerRouteReady('peer'), true);
    await assert.rejects(
      runtime.requestToNode('peer', payload, 1_000).promise,
      (error) =>
        error instanceof ZLinkBackendResultError &&
        error.operation === 'request' &&
        error.result === RequestResult.Busy
    );
    assert.deepEqual(requested, ['peer']);
  } finally {
    runtime.close();
  }
});

for (const admittedGeneration of [undefined, 11n]) {
  test(`unadmitted route records are discarded without processing or replies (${admittedGeneration})`, async () => {
    const h = selectedRouteRuntime(new Map([['peer', 12n]]));
    let dispatches = 0;
    let replies = 0;
    h.runtime.serviceIngress = () => {
      dispatches++;
      return 'infrastructure';
    };
    try {
      if (admittedGeneration !== undefined)
        h.runtime.admitPeer(descriptor('peer'), admittedGeneration, 0);
      for (const parts of [
        [
          Buffer.from(
            wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.update, descriptor('peer'))
          )
        ],
        [Buffer.from([0])],
        [Buffer.from(wire.encodeNodeRequestHeader(1n)), Buffer.from([0])]
      ]) {
        const result = await h.processReceived({
          sourceRid: 'peer',
          routeGeneration: 12n,
          parts,
          requestSeq: 1n,
          reply() {
            replies++;
          }
        });
        assert.equal(result, 'dropped');
      }
      assert.equal(dispatches, 0);
      assert.equal(replies, 0);
    } finally {
      h.runtime.close();
    }
  });
}
