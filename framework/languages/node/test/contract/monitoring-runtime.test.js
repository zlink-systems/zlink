const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const internalEvents = require('../../packages/framework/dist/runtime/diagnostics/internal-event-contracts');
const spotContextRuntime = require('../../packages/framework/dist/runtime/spots/spot-context');
const spotTimerRuntime = require('../../packages/framework/dist/runtime/spots/spot-timer');

test('runtime event publisher continues after a monitoring handler fails', async () => {
  const events = [];
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle() { throw new Error('handler failed'); } });
  publisher.register({ async handle(event) { events.push(event); } });
  const originalError = console.error;
  console.error = () => undefined;
  try {
    await publisher.publish({ sourceName: 'runtime', timestamp: new Date(), event: 'test' });
  } finally {
    console.error = originalError;
  }

  assert.equal(events.length, 1);
});

test('socket monitoring source maps backend raw events into framework typed events', async () => {
  const events = [];
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  const source = new framework.ZLinkSocketMonitoringSource(
    {
      sourceName: 'api.server',
      events: [internalEvents.ZLinkSocketEventKind.ConnectionReady]
    },
    fakeSocketMonitor(),
    publisher
  );

  await source.publish({
    nativeEvent: internalEvents.ZLinkSocketNativeEventType.Connected,
    routingId: 'peer-a',
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://remote',
    value: 1
  });
  const opaqueRoutingId = {
    toHex() { return '706565722d61'; },
    toString() { return 'opaque backend object'; }
  };
  await source.publish({
    nativeEvent: internalEvents.ZLinkSocketNativeEventType.ConnectionReady,
    routingId: opaqueRoutingId,
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://remote',
    value: 2
  });

  assert.equal(events.length, 1);
  assert.equal(events[0].sourceName, 'api.server');
  assert.equal(events[0].event, internalEvents.ZLinkSocketEventKind.ConnectionReady);
  assert.equal(events[0].routingId, '706565722d61');
  assert.equal(typeof events[0].routingId, 'string');
});

test('location runtime monitoring source publishes snapshot changes and suppresses unchanged polls', async () => {
  const events = [];
  let readyCount = 1;
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  const source = new framework.ZLinkLocationRuntimeMonitoringSource(
    { sourceName: 'location-runtime', intervalMs: 1000 },
    {
      async getStatus() {
        return {
          storeHealthy: true,
          watchEnabled: false,
          pollingIntervalMs: 1000,
          lastRefreshAt: new Date(readyCount),
          ownerLeaseHealthy: true,
          ownerLeaseRenewedAt: new Date(readyCount)
        };
      },
      async listTopology() {
        return { items: [locationTopologyEntry(readyCount)] };
      },
      async listServiceSummaries() {
        return { items: [{
          meshName: 'api',
          totalCount: 1,
          readyCount,
          errorCount: 0,
          stoppedCount: 0,
          lastUpdatedAt: new Date(readyCount)
        }] };
      },
      async listPeerLocations() {
        return [];
      },
      async listSpotLocations() {
        return { items: [] };
      },
      async listActorLocations() {
        return { items: [] };
      },
      async listRouteLocations() {
        return { items: [] };
      }
    },
    publisher
  );

  await source.pollOnce();
  await source.pollOnce();
  readyCount = 2;
  await source.pollOnce();

  assert.deepEqual(events.map((event) => event.event), [
    internalEvents.ZLinkLocationRuntimeEventKind.StatusChanged,
    internalEvents.ZLinkLocationRuntimeEventKind.TopologyChanged,
    internalEvents.ZLinkLocationRuntimeEventKind.ServiceSummaryChanged,
    internalEvents.ZLinkLocationRuntimeEventKind.StatusChanged,
    internalEvents.ZLinkLocationRuntimeEventKind.TopologyChanged,
    internalEvents.ZLinkLocationRuntimeEventKind.ServiceSummaryChanged
  ]);
  assert.equal(events[2].serviceSummary[0].readyCount, 1);
  assert.equal(events[5].serviceSummary[0].readyCount, 2);
});

test('location runtime monitoring source publishes StoreFailure and StoreRecovered', async () => {
  const events = [];
  let fail = true;
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  const query = {
    async getStatus() {
      if (fail) throw new Error('store unavailable');
      return {
        storeHealthy: true,
        watchEnabled: false,
        pollingIntervalMs: 1000,
        ownerLeaseHealthy: true
      };
    },
    async listTopology() { return { items: [] }; },
    async listServiceSummaries() { return { items: [] }; }
  };
  const source = new framework.ZLinkLocationRuntimeMonitoringSource(
    { sourceName: 'location-runtime', intervalMs: 1000 },
    query,
    publisher
  );

  await source.pollOnce();
  await source.pollOnce();
  fail = false;
  await source.pollOnce();

  assert.equal(internalEvents.ZLinkLocationRuntimeEventKind.StoreUnavailable, undefined);
  assert.deepEqual(events.slice(0, 2).map((event) => event.event), [
    internalEvents.ZLinkLocationRuntimeEventKind.StoreFailure,
    internalEvents.ZLinkLocationRuntimeEventKind.StoreRecovered
  ]);
});

test('location monitoring event emitter publishes registered row and resolve-miss events', async () => {
  const events = [];
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  const emitter = new framework.ZLinkLocationMonitoringEventEmitter({
    peer: { sourceName: 'location-peer' },
    spot: { sourceName: 'location-spot' },
    actor: { sourceName: 'location-actor' },
    route: { sourceName: 'location-route' }
  }, publisher);

  emitter.peerRowUpdated({
    kind: framework.ZLinkLocationKind.Peer,
    key: {
      autoConnectType: framework.ZLinkLocationAutoConnectType.RouteMesh,
      meshName: 'api',
      role: framework.ZLinkLocationRole.Router,
      endpoint: 'tcp://127.0.0.1:7001'
    }
  }, peerRow());
  emitter.spotResolveMiss({ meshName: 'game', spotId: 'spot-1' });
  emitter.actorResolveMiss({ actorType: 'GameActor', actorId: 'room-1' });
  emitter.routeRowRemoved({ routeKind: framework.ZLinkRouteKind.FrameworkRoute, routeKey: 'api' });
  await Promise.resolve();

  assert.deepEqual(events.map((event) => [event.sourceName, event.event]), [
    ['location-peer', internalEvents.ZLinkLocationPeerEventKind.RowUpdated],
    ['location-spot', internalEvents.ZLinkLocationSpotEventKind.ResolveMiss],
    ['location-actor', internalEvents.ZLinkLocationActorEventKind.ResolveMiss],
    ['location-route', internalEvents.ZLinkLocationRouteEventKind.RowRemoved]
  ]);
  assert.equal(events[0].peer.endpoint, 'tcp://127.0.0.1:7001');
  assert.equal(events[1].key.meshName, 'game');
  assert.equal(events[2].key.actorId, 'room-1');
  assert.equal(events[3].key.routeKey, 'api');
});

test('continuing spot timer publishes one TimerHandlerFailed event with its SpotId', async () => {
  const events = [];
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  class IdleTimerHandler {}
  const timer = new framework.ZLinkManagedTimer(
    'idle',
    5,
    {
      overrunPolicy: framework.ZLinkTimerOverrunPolicy.DelayNextTick,
      maxCatchUpTicks: 1,
      stopOnUnhandledException: false
    },
    async () => {
      throw new TypeError('timer failed');
    },
    spotTimerRuntime.createTimerDiagnostics(
      'stage-node',
      'stage-entry-550e8400-e29b-41d4-a716-446655440000',
      true,
      'idle',
      IdleTimerHandler,
      publisher
    )
  );

  try {
    await waitFor(() => events.length >= 1, 1000);
  } finally {
    await timer.dispose();
  }

  assert.deepEqual(events.map((event) => event.event), [internalEvents.ZLinkSpotEventKind.TimerHandlerFailed]);
  assert.equal(events[0].timerDiagnostic.spotId, 'stage-entry-550e8400-e29b-41d4-a716-446655440000');
  assert.equal(events[0].timerDiagnostic.timerName, 'idle');
  assert.equal(events[0].timerDiagnostic.exceptionType, 'TypeError');
});

test('Entry Spot timer diagnostic uses the context SpotId rather than NodeRid', async () => {
  const events = [];
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  let reportFailure;
  const context = spotContextRuntime.createEntrySpotContext({
    spotId: 'stage-entry-550e8400-e29b-41d4-a716-446655440000',
    objectGeneration: 1,
    nodeRid: 'stage-node-rid',
    handlers: {},
    outbound: {},
    timers: {
      async add(_name, _periodMs, _timerOptions, _handlerType, _serial, _spot, _resolver, _signal, reporter) {
        reportFailure = reporter;
        return {};
      }
    },
    serial: {},
    getEntrySpot: () => ({}),
    spotNodeName: 'stage',
    runtimeEventPublisher: publisher,
    workerRuntime: {}
  });
  class IdleTimerHandler {}

  await context.addTimer('idle', 1000, IdleTimerHandler);
  await reportFailure(
    { deliveryIndex: 1n, scheduledIndex: 1n },
    new TypeError('timer failed'),
    internalEvents.ZLinkSpotEventKind.TimerHandlerFailed
  );

  assert.equal(context.spotId, 'stage-entry-550e8400-e29b-41d4-a716-446655440000');
  assert.equal(events[0].timerDiagnostic.spotId, context.spotId);
  assert.notEqual(events[0].timerDiagnostic.spotId, context.nodeRid);
});

test('decorated Entry Spot timer registration reports the canonical Entry SpotId', async () => {
  const events = [];
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  class EntrySpot {}
  class IdleTimerHandler {}
  let reportFailure;
  const timers = {
    async add(_name, _periodMs, _options, _handlerType, _serial, _spot, _resolver, _signal, reporter) {
      reportFailure = reporter;
      return {};
    }
  };

  await spotTimerRuntime.addEntrySpotTimerRegistrations(
    timers,
    EntrySpot,
    new EntrySpot(),
    {},
    { timerHandlers: [{
      entrySpotType: EntrySpot,
      handlerType: IdleTimerHandler,
      name: 'idle',
      periodMs: 1000
    }] },
    {
      spotNodeName: 'stage',
      spotId: 'stage-entry-550e8400-e29b-41d4-a716-446655440000',
      runtimeEventPublisher: publisher
    }
  );
  await reportFailure(
    { deliveryIndex: 1n, scheduledIndex: 1n },
    new TypeError('timer failed'),
    internalEvents.ZLinkSpotEventKind.TimerHandlerFailed
  );

  assert.equal(events[0].timerDiagnostic.spotId, 'stage-entry-550e8400-e29b-41d4-a716-446655440000');
  assert.notEqual(events[0].timerDiagnostic.spotId, 'stage-node-rid');
});

test('stopping spot timer publishes only TimerStoppedAfterUnhandledException', async () => {
  const events = [];
  const publisher = new framework.DefaultZLinkRuntimeEventPublisher();
  publisher.register({ async handle(event) { events.push(event); } });
  class IdleTimerHandler {}
  const timer = new framework.ZLinkManagedTimer(
    'idle',
    1,
    {
      overrunPolicy: framework.ZLinkTimerOverrunPolicy.DelayNextTick,
      maxCatchUpTicks: 1,
      stopOnUnhandledException: true
    },
    async () => {
      throw new TypeError('timer stopped');
    },
    spotTimerRuntime.createTimerDiagnostics(
      'stage-node',
      'stage-entry-550e8400-e29b-41d4-a716-446655440000',
      true,
      'idle',
      IdleTimerHandler,
      publisher
    )
  );

  try {
    await waitFor(() => timer.isDisposed, 1000);
  } finally {
    await timer.dispose();
  }

  assert.deepEqual(events.map((event) => event.event), [
    internalEvents.ZLinkSpotEventKind.TimerStoppedAfterUnhandledException
  ]);
  assert.equal(events[0].timerDiagnostic.exceptionMessage, 'timer stopped');
});

function fakeSocketMonitor() {
  return {
    nativeInstance: {},
    onEvent() {},
    recv() {},
    async dispose() {}
  };
}

function locationTopologyEntry(readyCount) {
  return {
    meshName: 'api',
    nodeRid: 'peer-a',
    endpoint: 'tcp://peer:7101',
    draining: false,
    state: framework.ZLinkLocationTopologyState.Ready,
    updatedAt: new Date(readyCount)
  };
}

function peerRow() {
  return {
    autoConnectType: framework.ZLinkLocationAutoConnectType.RouteMesh,
    meshName: 'api',
    role: framework.ZLinkLocationRole.Router,
    endpoint: 'tcp://127.0.0.1:7001',
    weight: 1,
    value: 0n,
    ownerId: 'owner-a',
    generation: 1n,
    updatedAt: new Date(1)
  };
}

async function waitFor(predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  assert.fail('Timed out waiting for predicate.');
}
