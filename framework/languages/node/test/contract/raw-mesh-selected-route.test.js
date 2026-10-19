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
  createServiceWireCodec
} = require('../../packages/framework/dist/runtime/foundation/service-wire-codec');
const livenessCodec = createServiceWireCodec({
  magic: wire.M6A_SERVICE_WIRE_MAGIC,
  major: wire.M6A_SERVICE_WIRE_MAJOR,
  commands: wire.M6aServiceWireCommand
});
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

function selectedRouteRuntime(routes, submit, options = {}) {
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
      sent.push([target, wire.decodeHeader(parts[0]).command, Buffer.from(parts[0])]);
      if (submit) return submit(target, parts);
      return { result: SubmitResult.Ok, admitted: Promise.resolve() };
    },
    async send(target, parts) {
      sent.push([target, wire.decodeHeader(parts[0]).command, Buffer.from(parts[0])]);
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
    ...options,
    descriptor: descriptor('local'),
    onPeerDisconnected: (rid) => admissionsEnded.push(rid),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  runtime.start();
  return {
    runtime,
    router,
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

for (const deferred of [false, true]) {
  test(`HELLO retains its first bytes until ADMIT and then publishes UPDATE (deferred=${deferred})`, async () => {
    let complete;
    const h = selectedRouteRuntime(
      new Map([['peer', 11n]]),
      deferred
        ? (_target, parts) =>
            wire.decodeHeader(parts[0]).command === wire.M6aServiceWireCommand.hello
              ? {
                  result: SubmitResult.Backpressured,
                  admitted: new Promise((resolve) => {
                    complete = resolve;
                  })
                }
              : { result: SubmitResult.Ok, admitted: Promise.resolve() }
        : undefined
    );
    const receive = (command) =>
      h.processReceived({
        sourceRid: 'peer',
        routeGeneration: 11n,
        parts: [wire.encodeRouteMeshAdmission(command, descriptor('peer'))]
      });
    try {
      h.runtime.connectPeerByRoutingId(descriptor('peer').advertisedEndpoint, 'peer');
      await h.runtime.observeSelectedRoutes();
      const first = h.sent[0][2];
      const revision = wire.decodeRouteMeshAdmission(
        first,
        wire.M6aServiceWireCommand.hello,
        'local'
      ).descriptorRevision;
      await h.runtime.updateLocalDescriptor({ channelName: 'alpha', channelWeight: 50 });
      if (deferred) {
        h.runtime.tickLiveness(0);
        assert.equal(h.sent.length, 1, 'pending submission is not submitted twice');
        complete();
        await turn();
      }
      h.runtime.tickLiveness(0);
      await turn();
      const hellos = h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello);
      assert.equal(hellos.length, 2);
      assert.deepEqual(hellos[1][2], first);
      complete?.();
      await turn();
      // A crossed HELLO must respond with the same admission snapshot.
      await receive(wire.M6aServiceWireCommand.hello);
      const admit = h.sent.find(([, command]) => command === wire.M6aServiceWireCommand.admit)[2];
      assert.equal(
        wire.decodeRouteMeshAdmission(admit, wire.M6aServiceWireCommand.admit, 'local')
          .descriptorRevision,
        revision
      );
      complete?.();
      await turn();
      const unsubscribe = h.runtime.observePeerRouteChanged((rid) => {
        void h.runtime.announcePeer(rid);
      });
      await receive(wire.M6aServiceWireCommand.admit);
      unsubscribe();
      const update = h.sent.find(
        ([, command]) => command === wire.M6aServiceWireCommand.update
      )?.[2];
      assert.ok(update, 'ADMIT publishes the current descriptor through UPDATE');
      assert.equal(
        wire.decodeRouteMeshAdmission(update, wire.M6aServiceWireCommand.update, 'local')
          .descriptorRevision,
        revision + 1n
      );
      complete?.();
      await turn();
      h.runtime.tickLiveness(0);
      assert.equal(
        h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello).length,
        2
      );
    } finally {
      complete?.();
      h.runtime.close();
    }
  });
}

for (const command of [wire.M6aServiceWireCommand.hello, wire.M6aServiceWireCommand.admit]) {
  test(`wire admission rejects an increased revision for command ${command}`, async () => {
    const h = selectedRouteRuntime(new Map([['peer', 11n]]));
    const receive = (revision) =>
      h.processReceived({
        sourceRid: 'peer',
        routeGeneration: 11n,
        parts: [
          wire.encodeRouteMeshAdmission(command, {
            ...descriptor('peer'),
            descriptorRevision: revision
          })
        ]
      });
    try {
      await h.runtime.observeSelectedRoutes();
      assert.equal(await receive(1n), 'infrastructure');
      assert.equal(await receive(2n), 'protocolError');
      assert.equal(h.runtime.topology.peer('peer').descriptor.descriptorRevision, 1n);
    } finally {
      h.runtime.close();
    }
  });
}

test('route replacement retires a pending HELLO snapshot before its send completes', async () => {
  const routes = new Map([['peer', 11n]]);
  let complete;
  let submitted = 0;
  const h = selectedRouteRuntime(routes, () =>
    ++submitted === 1
      ? {
          result: SubmitResult.Backpressured,
          admitted: new Promise((resolve) => {
            complete = resolve;
          })
        }
      : { result: SubmitResult.Ok, admitted: Promise.resolve() }
  );
  try {
    await h.runtime.observeSelectedRoutes();
    await h.runtime.updateLocalDescriptor({ channelName: 'alpha', channelWeight: 50 });
    routes.set('peer', 12n);
    await h.runtime.observeSelectedRoutes();
    const replacement = h.sent.at(-1)[2];
    assert.notDeepEqual(replacement, h.sent[0][2]);
    complete();
    await turn();
    h.runtime.tickLiveness(0);
    assert.deepEqual(
      h.sent.at(-1)[2],
      replacement,
      'old completion cannot retire the new exchange'
    );
    routes.clear();
    await h.runtime.observeSelectedRoutes();
    assert.equal(h.runtime.controlSends.size, 0);
  } finally {
    complete?.();
    h.runtime.close();
  }
  assert.equal(h.runtime.controlSends.size, 0);
});

test('RouteMesh admission follows the observed Core selected route (FW01)', async () => {
  const peer = descriptor('peer');
  const routes = new Map([['peer', 11n]]);
  const harness = selectedRouteRuntime(routes);
  const { runtime, sent, processReceived } = harness;
  const admit = (routeGeneration) =>
    processReceived({
      sourceRid: 'peer',
      routeGeneration,
      parts: [Buffer.from(wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.admit, peer))]
    });
  try {
    runtime.connectPeerByRoutingId(peer.advertisedEndpoint, 'peer');
    await runtime.observeSelectedRoutes();
    assert.equal(
      sent.filter(
        ([target, command]) => target === 'peer' && command === wire.M6aServiceWireCommand.hello
      ).length,
      1
    );

    // Core delivers the handshake of the selected route.
    assert.equal(await admit(11n), 'infrastructure');
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
    await runtime.observeSelectedRoutes();
    assert.equal(runtime.topology.peer('peer'), undefined);
    assert.ok(
      sent.some(
        ([target, command]) => target === 'peer' && command === wire.M6aServiceWireCommand.hello
      )
    );
    assert.equal(await admit(12n), 'infrastructure');
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
        [],
        [Buffer.alloc(0)],
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

test('inbound selected route sends HELLO without an outbound intent', async () => {
  const h = selectedRouteRuntime(new Map([['peer', 11n]]));
  try {
    await h.runtime.observeSelectedRoutes();
    assert.equal(h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello).length, 1);
  } finally { h.runtime.close(); }
});
// MeshNode §7.1 and hot-path §4.2: an inbound-only route must retain its
// management wake until ADMIT, even after a crossed HELLO admits its descriptor.
for (const crossedHello of [false, true]) {
  test(`maintenance stays due until inbound selected route receives ADMIT (crossedHello=${crossedHello})`, async (t) => {
    const now = 1000;
    t.mock.method(performance, 'now', () => now);
    const h = selectedRouteRuntime(new Map([['peer', 11n]]));
    const receive = (command) => h.processReceived({
      sourceRid: 'peer',
      routeGeneration: 11n,
      parts: [wire.encodeRouteMeshAdmission(command, descriptor('peer'))]
    });
    try {
      await h.runtime.observeSelectedRoutes();
      if (crossedHello) await receive(wire.M6aServiceWireCommand.hello);
      h.runtime.liveness.tick(now);
      assert.equal(h.runtime.nextMaintenanceDeadlineMs(now), now);
      const hellos = () => h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello).length;
      const initial = hellos();
      h.runtime.tickLiveness(now);
      assert.equal(hellos(), initial + 1);
      await receive(wire.M6aServiceWireCommand.admit);
      h.runtime.liveness.tick(now);
      assert.ok(h.runtime.nextMaintenanceDeadlineMs(now) > now);
      h.runtime.tickLiveness(now);
      assert.equal(hellos(), initial + 1);
    } finally {
      h.runtime.close();
    }
  });
}

test('liveness maintenance resends HELLO until the selected route receives ADMIT', async () => {
  const h = selectedRouteRuntime(new Map([['peer', 11n]]));
  try {
    h.runtime.connectPeerByRoutingId(descriptor('peer').advertisedEndpoint, 'peer');
    await h.runtime.observeSelectedRoutes();
    const initial = h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello).length;
    h.runtime.tickLiveness(0);
    await turn();
    assert.equal(h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello).length, initial + 1);
  } finally { h.runtime.close(); }
});

for (const beforeSnapshot of [false, true]) {
  test(`remote ADMIT stops HELLO and resets only on route replacement (beforeSnapshot=${beforeSnapshot})`, async () => {
    const routes = new Map([['peer', 11n]]);
    const h = selectedRouteRuntime(routes);
    const receive = (command, routeGeneration) => h.processReceived({
      sourceRid: 'peer', routeGeneration,
      parts: [wire.encodeRouteMeshAdmission(command, descriptor('peer'))]
    });
    const hellos = () => h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello).length;
    try {
      if (!beforeSnapshot) await h.runtime.observeSelectedRoutes();
      await receive(wire.M6aServiceWireCommand.admit, 11n);
      await h.runtime.observeSelectedRoutes();
      const accepted = hellos();
      await receive(wire.M6aServiceWireCommand.hello, 11n);
      await receive(wire.M6aServiceWireCommand.update, 11n);
      h.runtime.tickLiveness();
      await turn();
      assert.equal(hellos(), accepted, 'HELLO and UPDATE must preserve the received ADMIT');
      routes.set('peer', 12n);
      // Core can deliver the new route handshake before the Framework observes its snapshot.
      if (beforeSnapshot) await receive(wire.M6aServiceWireCommand.admit, 12n);
      await h.runtime.observeSelectedRoutes();
      assert.equal(hellos(), accepted + (beforeSnapshot ? 0 : 1));
      if (!beforeSnapshot) {
        await receive(wire.M6aServiceWireCommand.hello, 12n);
        h.runtime.tickLiveness();
        await turn();
        assert.equal(hellos(), accepted + 2, 'receiving HELLO alone must not stop HELLO submission');
        await receive(wire.M6aServiceWireCommand.admit, 12n);
      }
      const completed = hellos();
      h.runtime.tickLiveness();
      await turn();
      assert.equal(hellos(), completed);
    } finally { h.runtime.close(); }
  });
}

test('peer route readiness wait uses existing admission notifications and removes listeners', async () => {
  const h = selectedRouteRuntime(new Map([['peer', 11n]]));
  const peer = descriptor('peer');
  try {
    const ready = h.runtime.waitForPeerRoute('peer', 1n, BigInt(Date.now() + 10000));
    assert.equal(h.runtime.pendingOperationCount, 1);
    h.runtime.connectPeerByRoutingId(peer.advertisedEndpoint, 'peer');
    await h.runtime.observeSelectedRoutes();
    await h.processReceived({ sourceRid: 'peer', routeGeneration: 11n,
      parts: [Buffer.from(wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.admit, peer))] });
    await ready;
    assert.equal(h.runtime.pendingOperationCount, 0);
    assert.equal(h.runtime.peerRouteChanged.size, 0);
    // A different lifecycle must wait even when the RID has a ready route.
    const stale = h.runtime.waitForPeerRoute('peer', 2n, BigInt(Date.now() + 10000));
    const rejected = assert.rejects(stale, { name: 'OperationTimeoutError' });
    h.runtime.expireOperations(performance.now() + 20000, performance.now() + 1000);
    await rejected;
    assert.equal(h.runtime.pendingOperationCount, 0);
    assert.equal(h.runtime.peerRouteChanged.size, 0);
  } finally { h.runtime.close(); }
});

for (const terminal of ['deadline', 'cancel', 'close']) {
  test(`peer route readiness ends on ${terminal} without a relocation timer`, async () => {
    const h = selectedRouteRuntime(new Map());
    const controller = new AbortController();
    try {
      const wait = h.runtime.waitForPeerRoute('missing', 1n, BigInt(Date.now() + 10000), controller.signal);
      const rejected = assert.rejects(wait);
      if (terminal === 'deadline') h.runtime.expireOperations(performance.now() + 20000, performance.now() + 1000);
      if (terminal === 'cancel') controller.abort(new Error('target stopped'));
      if (terminal === 'close') h.runtime.close();
      await rejected;
      assert.equal(h.runtime.pendingOperationCount, 0);
      assert.equal(h.runtime.peerRouteChanged.size, 0);
    } finally { h.runtime.close(); }
  });
}

// Hot-path §4.2: the registry owns the deadline; an admitted route has no
// HELLO exchange to maintain between probe/timeout deadlines.
test('idle liveness maintenance does not enumerate routes or control sends', async (t) => {
  t.mock.method(performance, 'now', () => 0);
  const h = selectedRouteRuntime(new Map([['peer', 11n]]));
  try {
    await h.runtime.observeSelectedRoutes();
    await h.processReceived({
      sourceRid: 'peer',
      routeGeneration: 11n,
      parts: [wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.admit, descriptor('peer'))]
    });
    assert.equal(h.runtime.tickLiveness(0).probes.length, 1);
    assert.equal(h.runtime.liveness.nextDeadlineMs(), 5000);
    assert.equal(h.runtime.controlSends.size, 0);
    const routes = t.mock.method(h.runtime.selectedRoutes, 'keys');
    const controls = t.mock.method(h.runtime.controlSends, 'values');
    const peers = t.mock.method(h.runtime.liveness.peers, Symbol.iterator);
    for (const now of [1, 1000, 4999]) {
      assert.deepEqual(h.runtime.tickLiveness(now), { probes: [], timedOutNodes: [] });
      assert.equal(h.runtime.nextMaintenanceDeadlineMs(now), 5000);
    }
    assert.equal(routes.mock.callCount(), 0);
    assert.equal(controls.mock.callCount(), 0);
    assert.equal(peers.mock.callCount(), 0);
  } finally {
    h.runtime.close();
  }
});

for (const requestCount of [1, 64]) {
  test(`a receive rotation maintains liveness once for ${requestCount} requests and once when idle`, async (t) => {
    t.mock.method(performance, 'now', () => 0);
    const h = selectedRouteRuntime(new Map([['peer', 11n]]));
    let received = 0;
    let closedRecords = 0;
    try {
      await h.runtime.observeSelectedRoutes();
      await h.processReceived({
        sourceRid: 'peer',
        routeGeneration: 11n,
        parts: [wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.admit, descriptor('peer'))]
      });
      h.runtime.tickLiveness(0);
      t.mock.method(performance, 'now', () => 1000);
      h.runtime.serviceIngress = () => {
        received++;
        return 'infrastructure';
      };
      t.mock.method(h.router, 'receive', () =>
        received < requestCount
          ? {
              sourceRid: 'peer',
              sourceRoute: Buffer.from('peer'),
              routeGeneration: 11n,
              requestSeq: BigInt(received + 1),
              parts: [wire.encodeNodeRequestHeader(BigInt(received + 1)), Buffer.from([0])],
              close() {
                closedRecords++;
              }
            }
          : undefined
      );
      const ticks = t.mock.method(h.runtime, 'tickLiveness');
      const routes = t.mock.method(h.runtime.selectedRoutes, 'keys');
      assert.equal(await h.runtime.pumpBatch(true), requestCount === 64);
      assert.equal(received, requestCount);
      assert.equal(closedRecords, requestCount);
      assert.equal(ticks.mock.callCount(), 1);
      assert.equal(await h.runtime.pumpBatch(true), false);
      assert.equal(ticks.mock.callCount(), 2);
      assert.equal(await h.runtime.pumpBatch(false), false);
      assert.equal(ticks.mock.callCount(), 3);
      assert.equal(routes.mock.callCount(), 0);
      h.runtime.close();
      await h.runtime.pumpBatch(false);
      assert.equal(ticks.mock.callCount(), 3);
    } finally {
      h.runtime.close();
    }
  });
}

test('maintenance preserves probe boundaries, ACK expiry and replacement admission', async (t) => {
  let now = 0;
  t.mock.method(performance, 'now', () => now);
  const routes = new Map([['peer', 11n]]);
  const h = selectedRouteRuntime(routes);
  const admit = (generation) =>
    h.processReceived({
      sourceRid: 'peer',
      routeGeneration: generation,
      parts: [wire.encodeRouteMeshAdmission(wire.M6aServiceWireCommand.admit, descriptor('peer'))]
    });
  try {
    await h.runtime.observeSelectedRoutes();
    await admit(11n);
    const probesSent = () =>
      h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.livenessProbe).length;
    const first = h.runtime.tickLiveness(0).probes[0];
    assert.equal(probesSent(), 1);
    assert.equal(h.runtime.tickLiveness(4999).probes.length, 0);
    assert.equal(probesSent(), 1);
    assert.equal(h.runtime.tickLiveness(5000).probes[0].probeId, first.probeId);
    assert.equal(probesSent(), 2);
    now = 6000;
    await h.processReceived({
      sourceRid: 'peer',
      routeGeneration: 11n,
      parts: [
        livenessCodec.encodeLivenessRecord({
          command: wire.M6aServiceWireCommand.livenessAck,
          probeId: first.probeId
        })
      ]
    });
    assert.equal(h.runtime.tickLiveness(9999).probes.length, 0);
    assert.notEqual(h.runtime.tickLiveness(10000).probes[0].probeId, first.probeId);
    assert.equal(probesSent(), 3);
    assert.deepEqual(h.runtime.tickLiveness(20999).timedOutNodes, []);
    assert.deepEqual(h.runtime.tickLiveness(21000).timedOutNodes, ['peer']);
    h.runtime.tickLiveness(21001);
    assert.deepEqual(h.disconnected, ['peer']);
    assert.deepEqual(h.admissionsEnded, ['peer']);
    now = 22000;
    routes.set('peer', 12n);
    await h.runtime.observeSelectedRoutes();
    await admit(12n);
    assert.equal(h.runtime.tickLiveness(now).probes[0].connectionId, 12n);
    assert.deepEqual(h.runtime.tickLiveness(36999).timedOutNodes, []);
    assert.deepEqual(h.runtime.tickLiveness(37000).timedOutNodes, ['peer']);
    assert.deepEqual(h.disconnected, ['peer', 'peer']);
  } finally {
    h.runtime.close();
  }
});

for (const initiallySealed of [false, true]) {
  test(`HELLO maintenance resumes after admission reopens (initiallySealed=${initiallySealed})`, async (t) => {
    t.mock.method(performance, 'now', () => 0);
    let sealed = initiallySealed;
    const h = selectedRouteRuntime(new Map([['peer', 11n]]), undefined, {
      peerAdmissionSealed: () => sealed
    });
    const hellos = () =>
      h.sent.filter(([, command]) => command === wire.M6aServiceWireCommand.hello);
    try {
      await h.runtime.observeSelectedRoutes();
      const before = hellos().length;
      sealed = true;
      await h.runtime.updateLocalDescriptor({ channelName: 'alpha', channelWeight: 42 });
      h.runtime.tickLiveness(0);
      assert.equal(hellos().length, before);
      assert.equal(h.runtime.nextMaintenanceDeadlineMs(0), Infinity);
      sealed = false;
      assert.equal(h.runtime.nextMaintenanceDeadlineMs(1000), 1000);
      h.runtime.tickLiveness(1000);
      assert.equal(hellos().length, before + 1);
      if (before !== 0) assert.deepEqual(hellos()[1][2], hellos()[0][2]);
    } finally {
      h.runtime.close();
    }
  });
}

test('a HELLO submission completed while admission is sealed remains resumable', async (t) => {
  t.mock.method(performance, 'now', () => 0);
  let sealed = false;
  let complete;
  const h = selectedRouteRuntime(
    new Map([['peer', 11n]]),
    () => ({
      result: SubmitResult.Backpressured,
      admitted: new Promise((resolve) => {
        complete = resolve;
      })
    }),
    { peerAdmissionSealed: () => sealed }
  );
  try {
    await h.runtime.observeSelectedRoutes();
    sealed = true;
    const pending = [...h.runtime.controlSends.values()][0].task;
    h.runtime.tickLiveness(0);
    assert.equal([...h.runtime.controlSends.values()][0]?.task, pending);
    assert.equal(h.sent.length, 1);
    complete();
    await Promise.resolve();
    await Promise.resolve();
    sealed = false;
    h.runtime.tickLiveness(1000);
    assert.equal(h.sent.length, 2);
  } finally {
    h.runtime.close();
    complete?.();
    await Promise.resolve();
  }
});
