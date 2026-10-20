'use strict';

const { mapZLinkRequestReply } = require('../../packages/framework/dist/runtime/messaging/request-result');

const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');

const detachedTaskRunner = new ZLinkRuntimeTaskRunner(
  new ZLinkRuntimeTaskErrorSink(),
  new AbortController().signal
);

function collector() {
  const records = [];
  const instruments = [];
  const observables = [];
  const instrument = (name, kind) => ({
    add(value, attributes) { records.push({ name, kind, value, attributes }); },
    record(value, attributes) { records.push({ name, kind, value, attributes }); }
  });
  const create = (name, kind, options) => {
    instruments.push({ name, kind, unit: options?.unit });
    return instrument(name, kind);
  };
  return {
    records,
    instruments,
    observables,
    provider: {
      getMeter(name) {
        assert.equal(name, 'zlink.framework');
        return {
          createCounter: (instrumentName, options) => create(instrumentName, 'counter', options),
          createUpDownCounter: (instrumentName, options) => create(instrumentName, 'updown', options),
          createHistogram: (instrumentName, options) => create(instrumentName, 'histogram', options),
          createObservableCounter(instrumentName, options) {
            instruments.push({ name: instrumentName, kind: 'observable_counter', unit: options?.unit });
            return { addCallback(callback) { observables.push({ name: instrumentName, callback }); } };
          },
          createObservableGauge(instrumentName, options) {
            instruments.push({ name: instrumentName, kind: 'gauge', unit: options?.unit });
            return { addCallback(callback) { observables.push({ name: instrumentName, callback }); } };
          }
        };
      }
    }
  };
}

function capacitySnapshot({ peak = 5n, waits = 7n, duration = 1.25 } = {}) {
  return {
    measurementEpoch: 4n,
    coreHwm: {
      configuredProfile: framework.ZLinkCoreHwmProfile.Balanced,
      effectiveBudgetBytes: 1_024n,
      totalAppliedHwmBytes: 900n,
      coreQueueAccountedBytes: 250n,
      applicationAccountedBytes: 50n,
      currentAccountedBytes: 300n,
      provisionalAccountedBytes: 0n,
      peakAccountedBytes: 450n,
      completionCurrentAccountedBytes: 25n,
      completionPeakAccountedBytes: 40n,
      completionPendingMessageCount: 1n,
      totalMessagingAccountedBytes: 325n,
      monitorQueueAppliedHwmBytes: 10n,
      monitorQueueAccountedBytes: 5n,
      totalInstanceAppliedHwmBytes: 20n,
      totalInstanceAccountedBytes: 8n,
      blockedRatioPpm: 125n,
      activeDirectionalQueueCount: 2n,
      activeCompletionDirectionalQueueCount: 1n,
      activeSendQueueCount: 1n,
      activeReceiveQueueCount: 1n,
      outstandingApplicationLeaseCount: 1n,
      retiredQueueCount: 0n,
      deferredOriginCreditBytes: 0n
    },
    applicationJobQueue: {
      configuredProfile: framework.ZLinkApplicationJobQueueProfile.Balanced,
      effectiveProcessorCount: 8n,
      effectiveMaxQueuedApplicationJobs: 1_024n,
      reservedSupplyPermits: 1n,
      queuedApplicationJobs: 2n,
      permitsInUse: 3n,
      peakPermitsInUse: peak,
      capacityWaiters: 1n,
      capacityWaitCount: waits,
      capacityWaitDurationSeconds: duration
    }
  };
}

test('RMETRIC-001 global OpenTelemetry no-op provider remains callable', () => {
  const metrics = new framework.ZLinkRuntimeMetrics();
  assert.equal(metrics.enabled(), true);
  metrics.count('zlink.mesh_node.request.timeouts');
  metrics.change('zlink.mesh_node.requests.inflight', 1);
  metrics.duration('zlink.mesh_node.request.duration', 0.01);
});

test('RMETRIC-006 server runtime exposes the exact 59-instrument spec25 catalog', () => {
  const { provider, instruments } = collector();
  new framework.ZLinkRuntimeMetrics(provider);
  const expected = new Map([
    ['zlink.stream.connections.active', ['updown', '{connection}']],
    ['zlink.stream.connections.opened', ['counter', '{connection}']],
    ['zlink.stream.connections.closed', ['counter', '{connection}']],
    ['zlink.mesh_node.peers.configured', ['gauge', '{peer}']],
    ['zlink.mesh_node.peers.connected', ['gauge', '{peer}']],
    ['zlink.mesh_node.peers.ready', ['gauge', '{peer}']],
    ['zlink.mesh_node.channels.ready_members', ['gauge', '{member}']],
    ['zlink.mesh_node.channel.selection_failures', ['counter', '{failure}']],
    ['zlink.mesh_node.requests.inflight', ['updown', '{request}']],
    ['zlink.mesh_node.request.duration', ['histogram', 's']],
    ['zlink.mesh_node.request.timeouts', ['counter', '{request}']],
    ['zlink.mesh_node.messages.dropped', ['counter', '{message}']],
    ['zlink.object.capacity.active', ['gauge', '{object}']],
    ['zlink.object.capacity.reserved', ['gauge', '{object}']],
    ['zlink.object.capacity.limit', ['gauge', '{object}']],
    ['zlink.spot.type.capacity.active', ['gauge', '{spot}']],
    ['zlink.spot.type.capacity.reserved', ['gauge', '{spot}']],
    ['zlink.spot.type.capacity.limit', ['gauge', '{spot}']],
    ['zlink.object.activation.active', ['gauge', '{activation}']],
    ['zlink.object.activation.limit', ['gauge', '{activation}']],
    ['zlink.spot.count', ['updown', '{spot}']],
    ['zlink.actor.count', ['updown', '{actor}']],
    ['zlink.relocation.started', ['counter', '{relocation}']],
    ['zlink.relocation.completed', ['counter', '{relocation}']],
    ['zlink.relocation.cutover_timeout', ['counter', '{fallback}']],
    ['zlink.relocation.duration', ['histogram', 's']],
    ['zlink.relocation.bytes', ['histogram', 'By']],
    ['zlink.relocation.interruption', ['histogram', 's']],
    ['zlink.relocation.target_resume', ['histogram', 's']],
    ['zlink.relocation.route_convergence', ['histogram', 's']],
    ['zlink.instance_spot.activations', ['counter', '{activation}']],
    ['zlink.instance_spot.activation.duration', ['histogram', 's']],
    ['zlink.instance_spot.pending.messages', ['gauge', '{message}']],
    ['zlink.instance_spot.pending.bytes', ['gauge', 'By']],
    ['zlink.instance_spot.claim.conflicts', ['counter', '{claim}']],
    ['zlink.instance_spot.takeovers', ['counter', '{takeover}']],
    ['zlink.location.store.errors', ['counter', '{error}']],
    ['zlink.location.owner_lease.renew.failures', ['counter', '{failure}']],
    ['zlink.location.owner_lease.renew.lateness', ['histogram', 's']],
    ['zlink.observability.events.overflow', ['counter', '{event}']],
    ['zlink.host.state', ['gauge', '{runtime}']],
    ['zlink.host.relocation.duration', ['histogram', 's']],
    ['zlink.host.relocation.blocked', ['counter', '{operation}']],
    ['zlink.host.shutdown.duration', ['histogram', 's']],
    ['zlink.host.shutdown.forced', ['counter', '{operation}']],
    ['zlink.host.core_hwm.effective_budget', ['gauge', 'By']],
    ['zlink.host.core_hwm.applied', ['gauge', 'By']],
    ['zlink.host.core_hwm.accounted', ['gauge', 'By']],
    ['zlink.host.core_hwm.completion_accounted', ['gauge', 'By']],
    ['zlink.host.core_hwm.blocked_ratio', ['gauge', '{ppm}']],
    ['zlink.host.application_job_queue.limit', ['gauge', '{job}']],
    ['zlink.host.application_job_queue.jobs', ['gauge', '{job}']],
    ['zlink.host.application_job_queue.capacity_waiters', ['gauge', '{waiter}']],
    ['zlink.host.application_job_queue.capacity_waits', ['observable_counter', '{wait}']],
    ['zlink.host.application_job_queue.capacity_wait_duration', ['observable_counter', 's']],
    ['zlink.host.application_job_queue.pressure_state', ['gauge', '{state}']],
    ['zlink.host.application_job_queue.pressure_transitions', ['observable_counter', '{transition}']],
    ['zlink.host.application_job_queue.pause_duration', ['gauge', 's']],
    ['zlink.host.application_job_queue.flow_state_config_failures', ['observable_counter', '{failure}']]
  ]);
  assert.equal(instruments.length, 59);
  assert.deepEqual(
    new Map(instruments.map(({ name, kind, unit }) => [name, [kind, unit]])),
    expected
  );
  assert(instruments.every(({ name }) => !name.startsWith('zlink.fanout.')));
});

test('application job queue pressure metrics expose bounded current and epoch series', () => {
  const { provider, observables } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  metrics.registerApplicationJobQueuePressure(() => ({
    pressureState: 'paused',
    runningTransitionCount: 2n,
    pausedTransitionCount: 3n,
    currentPauseDurationSeconds: 1.5,
    cumulativePauseDurationSeconds: 4.25,
    flowStateConfigFailureCount: 1n
  }));
  const samples = [];
  for (const observable of observables) {
    if (!observable.name.includes('application_job_queue.pressure')
        && !observable.name.includes('application_job_queue.pause_duration')
        && !observable.name.includes('application_job_queue.flow_state')) continue;
    observable.callback({
      observe(value, attributes) { samples.push({ name: observable.name, value, attributes }); }
    });
  }
  assert.deepEqual(samples, [
    {
      name: 'zlink.host.application_job_queue.pressure_state',
      value: 1,
      attributes: { state: 'paused' }
    },
    {
      name: 'zlink.host.application_job_queue.pause_duration',
      value: 1.5,
      attributes: { state: 'current' }
    },
    {
      name: 'zlink.host.application_job_queue.pause_duration',
      value: 4.25,
      attributes: { state: 'cumulative' }
    },
    {
      name: 'zlink.host.application_job_queue.pressure_transitions',
      value: 2,
      attributes: { state: 'running' }
    },
    {
      name: 'zlink.host.application_job_queue.pressure_transitions',
      value: 3,
      attributes: { state: 'paused' }
    },
    {
      name: 'zlink.host.application_job_queue.flow_state_config_failures',
      value: 1,
      attributes: undefined
    }
  ]);
});

test('RMETRIC-017 capacity instruments observe only the host capacity projection', () => {
  const { provider, observables } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  let snapshot = capacitySnapshot();
  metrics.registerHostCapacity(() => snapshot);

  const collect = () => {
    const samples = [];
    for (const observable of observables) {
      if (!observable.name.startsWith('zlink.host.core_hwm.')
          && !observable.name.startsWith('zlink.host.application_job_queue.')) continue;
      observable.callback({
        observe(value, attributes) { samples.push({ name: observable.name, value, attributes }); }
      });
    }
    return samples;
  };

  const beforeReset = collect();
  assert(beforeReset.some((sample) => sample.name === 'zlink.host.core_hwm.effective_budget'
    && sample.value === 1_024));
  assert(beforeReset.some((sample) => sample.name === 'zlink.host.core_hwm.accounted'
    && sample.value === 300
    && sample.attributes.state === 'current'));
  assert(beforeReset.some((sample) => sample.name === 'zlink.host.core_hwm.accounted'
    && sample.value === 450
    && sample.attributes.state === 'peak'));
  assert(beforeReset.some((sample) => sample.name === 'zlink.host.application_job_queue.jobs'
    && sample.value === 3
    && sample.attributes.state === 'in_use'));
  assert(beforeReset.some((sample) => sample.name === 'zlink.host.application_job_queue.capacity_waits'
    && sample.value === 7));
  assert(beforeReset.some((sample) => sample.name === 'zlink.host.application_job_queue.capacity_wait_duration'
    && sample.value === 1.25));
  assert(beforeReset.every((sample) => sample.attributes === undefined
    || Object.keys(sample.attributes).every((key) => key === 'state')));

  snapshot = capacitySnapshot({ peak: 3n, waits: 0n, duration: 0 });
  const afterReset = collect();
  assert(afterReset.some((sample) => sample.name === 'zlink.host.application_job_queue.jobs'
    && sample.value === 3
    && sample.attributes.state === 'peak'));
  assert(afterReset.some((sample) => sample.name === 'zlink.host.application_job_queue.capacity_waits'
    && sample.value === 0));
  assert.equal(
    new Set(afterReset.map((sample) => sample.name)).size,
    10
  );
});

// Spec stream-connector 32 no longer asks the client connector to carry
// instrumentation, so the connector has no meter provider option and emits no
// `zlink.stream.*` records. What remains here is the server runtime catalog.
test('RMETRIC-007 server catalog does not publish connector-only session bind metrics', async () => {
  const { provider, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  const socket = {
    send() { return true; },
    async submit() {},
    disconnectPeer() {},
    async bindActor() {},
    async unbindActor() {},
    sendBoundActor() { return true; }
  };
  const binding = new framework.ZLinkStreamBindingRuntime({ metrics });
  const context = binding.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-1'));

  await context.actors.bindOrGet({ nodeRid: 'node-1', actorId: 'actor-1', generation: 1n });

  assert.equal(records.some((record) => record.name === 'zlink.stream.session.bind.duration'), false);
});

test('RMETRIC Entry Spot activation records entry count and lifecycle counters', async () => {
  const { provider, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  class EntrySpot {
    async onJoinedActor() {}
    async onLeaveActor() {}
  }
  const activation = new framework.ZLinkEntrySpotActivation({
    detachedTaskRunner: detachedTaskRunner,
    entrySpotType: EntrySpot,
    nativeSpot: {
      routingId: 'entry-spot',
      async dispose() {}
    },
    nativeNode: { routingId: 'node-1' },
    nodeRid: 'node-1',
    spotNodeName: 'play',
    metrics
  });

  await activation.create();
  await activation.configure();
  await activation.initialize();
  await activation.dispose();

  assert.deepEqual(records.map(({ name, kind, value, attributes }) => ({
    name,
    kind,
    value,
    attributes
  })), [
    { name: 'zlink.spot.count', kind: 'updown', value: 1, attributes: { kind: 'entry' } },
    { name: 'zlink.spot.count', kind: 'updown', value: -1, attributes: { kind: 'entry' } }
  ]);
});

test('RMETRIC-009 classic fanout drops are excluded from mesh-node drop metrics', async () => {
  const { provider, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  const reporter = new framework.ZLinkDispatchErrorReporter(
    undefined,
    undefined,
    { reportRuntimeTaskException() {} },
    {
      diagnostics: { messageFlow: 'off', sampleRate: 1, includeMessageSizes: false },
      liveMode: { mode: 'off' }
    },
    metrics
  );
  const dispatcher = new framework.ZLinkChannelPublishDispatcher({
    channelName: 'events',
    dispatchErrors: reporter,
    handlers: new Map(),
    metrics
  });
  const parts = channelEnvelope.encodeChannelEnvelopeParts(
    4,
    'events',
    'MissingEvent',
    { value: 'payload' }
  ).map((part) => ({ data: () => Buffer.from(part) }));

  await dispatcher.dispatch({ topic: 'known', parts });

  assert.equal(
    records.some((record) => record.name === 'zlink.mesh_node.messages.dropped'),
    false
  );
});

test('RMETRIC-015 off diagnostics do not emit flow records or overflow metrics', () => {
  const { provider, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  const tracer = new framework.ZLinkMessageFlowTracer({
    diagnostics: { messageFlow: 'off', sampleRate: 1, includeMessageSizes: false },
    liveMode: { mode: 'off' }
  }, { reportRuntimeTaskException() {} }, metrics);

  assert.equal(framework.flowIfEnabled(tracer, 'received'), undefined);
  assert.equal(records.filter((record) => record.name === 'zlink.observability.events.overflow').length, 0);
});

test('OBS-B2/B3 observable gauges use bounded labels and isolate snapshot failures', () => {
  const { provider, observables } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  metrics.registerMeshSnapshots(() => { throw new Error('observer failure'); });
  const registration = metrics.registerMeshSnapshots(() => [{
    meshName: 'mesh-a', source: 'manual_and_redis',
    configuredPeers: 3, connectedPeers: 2, readyPeers: 1,
    channels: [{ channelName: 'orders', readyMembers: 4 }],
    actorCapacity: { active: 10, reserved: 2, limit: 100 },
    spotCapacity: { active: 3, reserved: 1, limit: 20 },
    spotTypeCapacities: [{
      spotKind: 'user', stableType: 'room', active: 2, reserved: 1, limit: 8
    }],
    activation: { active: 2, limit: 16 },
    instanceSpots: [{ instanceSpotType: 'matchmaker', pendingMessages: 7, pendingBytes: 4096 }]
  }]);
  const samples = [];
  for (const observable of observables) {
    observable.callback({
      observe(value, attributes) { samples.push({ name: observable.name, value, attributes }); }
    });
  }
  assert(samples.some((sample) => sample.name === 'zlink.mesh_node.peers.configured'
    && sample.value === 3
    && sample.attributes.source === 'manual_and_redis'));
  assert(samples.some((sample) => sample.name === 'zlink.spot.type.capacity.limit'
    && sample.value === 8
    && sample.attributes.stable_type === 'room'));
  assert(samples.every((sample) =>
    !['actor_id', 'spot_id', 'flow_id', 'correlation_id'].some((key) => key in sample.attributes)));
  registration.dispose();
});


test('capacity repository starts without a confirmed Store observation', () => {
  const provider = new framework.ZLinkInMemoryProviderLocationStore();
  const repository = new (require('../../packages/framework/dist/runtime/locations/location-store-repository').ZLinkLocationStoreRepository)(provider);
  assert.equal(typeof repository.confirmedCapacity, 'function');
  assert.equal(repository.confirmedCapacity({ meshName: 'unobserved', rid: 'local' }), undefined);
});


test('shared fixture defines capacity and takeover catalog and observation labels', () => {
  const fixture = require('../../../../runtime/protocol/fixtures/runtime-metrics-v1.json');
  const selected = fixture.metrics.filter(row => row.name.startsWith('zlink.object.capacity.') || row.name.startsWith('zlink.spot.type.capacity.') || row.name === 'zlink.instance_spot.takeovers' || row.name === 'zlink.mesh_node.request.duration');
  const { provider, instruments, observables, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  metrics.registerMeshSnapshots(() => [{
    meshName: 'fixture', source: 'manual', configuredPeers: 0, connectedPeers: 0, readyPeers: 0, channels: [],
    actorCapacity: { active: 1, reserved: 2, limit: 8 }, spotCapacity: { active: 1, reserved: 2, limit: 8 },
    spotTypeCapacities: [{ spotKind: 'user', stableType: 'room', active: 1, reserved: 2, limit: 8 }],
    activation: { active: 0, limit: 1 }, instanceSpots: []
  }]);
  for (const observable of observables) {
    if (!selected.some(row => row.name === observable.name)) continue;
    observable.callback({ observe(value, attributes) { records.push({ name: observable.name, value, attributes }); } });
  }
  metrics.startRequest('fixture', 'channel').complete('completed');
  for (const outcome of ['claimed', 'lost', 'failed']) metrics.recordInstanceSpotTakeover('fixture', 'room', outcome);
  for (const row of selected) {
    const instrument = instruments.find(value => value.name === row.name);
    assert(instrument, row.name);
    assert.equal(instrument.kind, row.kind === 'observable' ? 'gauge' : row.kind, row.name);
    assert.equal(instrument.unit, row.unit, row.name);
    const samples = records.filter(record => record.name === row.name);
    assert(samples.length > 0, row.name);
    for (const sample of samples) assert.deepEqual(Object.keys(sample.attributes).sort(), [...row.labels].sort(), row.name);
  }
});

test('capacity population series are absent before the first Store observation', () => {
  const { provider, observables } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  metrics.registerMeshSnapshots(() => [{
    meshName: 'unobserved', source: 'redis', configuredPeers: 0, connectedPeers: 0, readyPeers: 0, channels: [],
    actorCapacity: undefined, spotCapacity: undefined, spotTypeCapacities: [],
    activation: { active: 0, limit: 1 }, instanceSpots: []
  }]);
  const samples = [];
  for (const observable of observables.filter(value => value.name.includes('.capacity.'))) observable.callback({ observe(value) { samples.push(value); } });
  assert.deepEqual(samples, []);
});

test('channel terminal metrics normalize all five outcomes including caller cancellation and missing target', async () => {
  const { ZLinkRuntimeRouteTransport } = require('../../packages/framework/dist/runtime/channels/channel-transports');
  const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const fixture = require('../../../../runtime/protocol/fixtures/runtime-metrics-v1.json');
  const expected = fixture.metrics.find(row => row.name === 'zlink.mesh_node.request.duration').labelValues.outcome;
  const { provider, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  for (const outcome of expected) {
    const caller = new AbortController();
    let parts;
    const node = { requestToChannel(_channel, requestParts) { parts = requestParts; return { high: 1n, low: 1n }; } };
    const table = { async submit(operation) {
      operation();
      if (outcome === 'cancelled') { caller.abort(); throw caller.signal.reason; }
      const terminalResult = outcome === 'timed_out' ? RequestResult.TimedOut : outcome === 'shutdown' ? RequestResult.Terminated : outcome === 'failed' ? RequestResult.NotFound : 0;
      const header = channelEnvelope.decodeChannelHeader(parts.map(part => ZLinkBufferMessage.from(part)));
      return { terminalResult, failureErrno: 0, parts: terminalResult === 0 ? channelEnvelope.encodeChannelReplyParts(header, { ok: true }).map(part => ZLinkBufferMessage.from(part)) : [] };
    }};
    const transport = new ZLinkRuntimeRouteTransport(() => undefined, undefined,
      () => ({ meshNode: () => node, meshCompletionTable: () => table }),
      undefined, undefined, undefined, metrics, () => false);
    const pending = mapZLinkRequestReply(transport.requestToChannel('mesh', 'orders', 'Probe', { value: 1 }, 1000, caller.signal));
    if (outcome === 'completed') assert.deepEqual(await pending, { ok: true });
    else await assert.rejects(pending);
  }
  assert.deepEqual(records.filter(row => row.name === 'zlink.mesh_node.request.duration').map(row => row.attributes.outcome), expected);
  assert.equal(records.filter(row => row.name === 'zlink.mesh_node.request.timeouts').length, 1);
});


test('host capacity metrics use the repository observation rather than the publication descriptor', () => {
  const registration = framework.createFrameworkRegistrationWithBuilder(builder => builder.addRouteMesh('metric-mesh').objects().server());
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
  host.routeMeshCoordinator.snapshot = () => ({ peers: [], channels: [], readyPeerCount: 0 });
  const zero = { active: 0, reserved: 0, limit: 12 };
  const descriptor = { rid: 'metric-node', populationCapacity: { actors: zero, spots: zero, spotTypes: [] } };
  host.spotNodeRuntime = { meshNodeDescriptor: () => descriptor };
  let confirmed;
  host.locationOwner.stores = { locationStore: { confirmedCapacity(key, limits) {
    assert.deepEqual(key, { meshName: 'metric-mesh', rid: 'metric-node' });
    assert.equal(limits, descriptor.populationCapacity);
    return confirmed;
  } } };
  assert.equal(host.runtimeMetricMeshSnapshots()[0].actorCapacity, undefined);
  confirmed = { actors: { active: 9, reserved: 2, limit: 12 }, spots: zero, spotTypes: [] };
  const snapshot = host.runtimeMetricMeshSnapshots()[0];
  assert.equal(snapshot.actorCapacity.active, 9);
  assert.equal(snapshot.actorCapacity.reserved, 2);
  assert.equal(descriptor.populationCapacity.actors.active, 0);
});


test('Store operation metrics classify owner claims and exclude normal conflicts', async () => {
  const sink = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(sink.provider);
  const { ZLinkLocationStoreRepository } = require('../../packages/framework/dist/runtime/locations/location-store-repository');
  const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
  const provider = new ZLinkInMemoryProviderLocationStore();
  const repository = new ZLinkLocationStoreRepository({
    read: provider.read.bind(provider), scan: provider.scan.bind(provider),
    async write() { throw new Error('backend unavailable'); }
  }, undefined, metrics);
  const runtime = new framework.ZLinkLocationRuntime({
    stores: { locationStore: repository, ownerLeaseStore: repository }, metrics,
    setTimer: () => 0, clearTimer() {}
  });
  await runtime.start('metric-owner');
  const operations = sink.records.filter(row => row.name === 'zlink.location.store.errors').map(row => row.attributes.operation);
  assert.deepEqual(operations, ['compare_exchange']);
  const fixture = require('../../../../runtime/protocol/fixtures/runtime-metrics-v1.json');
  const allowed = fixture.metrics.find(row => row.name === 'zlink.location.store.errors').labelValues.operation;
  for (const operation of operations) assert.ok(allowed.includes(operation), operation);
});


function assertStoreOperations(sink, expected) {
  const operations = sink.records.filter(row => row.name === 'zlink.location.store.errors').map(row => row.attributes.operation);
  assert.deepEqual(operations, expected);
  const allowed = require('../../../../runtime/protocol/fixtures/runtime-metrics-v1.json').metrics
    .find(row => row.name === 'zlink.location.store.errors').labelValues.operation;
  for (const operation of operations) assert.ok(allowed.includes(operation), operation);
}

for (const [method, args, operation] of [
  ['readOwnerLease', ['owner'], 'read'],
  ['claimOwnerLease', ['owner', 15000], 'compare_exchange'],
  ['renewOwnerLease', [{ ownerId: 'owner', leaseGeneration: 1n }, 15000], 'lease_renew'],
  ['releaseOwnerLease', [{ ownerId: 'owner', leaseGeneration: 1n }], 'release'],
  ['removeMeshNode', [{ meshName: 'mesh', rid: 'node' }, { ownerId: 'owner', leaseGeneration: 1n }], 'release'],
  ['removeAllByOwner', [{ ownerId: 'owner', leaseGeneration: 1n }], 'release'],
  ['readAuthority', [{ value: 'zla1:a:9:authority' }], 'read']
]) {
  test(`Store backend failure ${method} records ${operation} once`, async () => {
    const sink = collector();
    const { ZLinkLocationStoreRepository } = require('../../packages/framework/dist/runtime/locations/location-store-repository');
    const fail = async () => { throw new Error('backend unavailable'); };
    const repository = new ZLinkLocationStoreRepository({ read: fail, write: fail, scan: fail }, undefined, new framework.ZLinkRuntimeMetrics(sink.provider));
    await assert.rejects(() => repository[method](...args));
    assertStoreOperations(sink, [operation]);
  });
}

for (const [method, args, operation] of [
  ['put', [{ value: 'reference' }, Buffer.from([1]), 15000], 'relocation_put'],
  ['read', [{ value: 'reference' }], 'relocation_get'],
  ['renew', [{ value: 'reference' }, 15000], 'relocation_put'],
  ['delete', [{ value: 'reference' }], 'relocation_delete'],
  ['putNew', [Buffer.from([1]), 15000], 'relocation_put']
]) {
  test(`Relocation backend failure ${method} records ${operation} once`, async () => {
    const sink = collector();
    const { ZLinkProviderRelocationRepository } = require('../../packages/framework/dist/runtime/locations/relocation-blob');
    const fail = async () => { throw new Error('backend unavailable'); };
    const repository = new ZLinkProviderRelocationRepository({ put: fail, read: fail, renew: fail, delete: fail }, new framework.ZLinkRuntimeMetrics(sink.provider));
    await assert.rejects(() => repository[method](...args));
    assertStoreOperations(sink, [operation]);
  });
  for (const Failure of [TypeError, RangeError]) {
    test(`Relocation ${method} ${Failure.name} does not count`, async () => {
      const sink = collector();
      const { ZLinkProviderRelocationRepository } = require('../../packages/framework/dist/runtime/locations/relocation-blob');
      const fail = async () => { throw new Failure('input'); };
      const repository = new ZLinkProviderRelocationRepository({ put: fail, read: fail, renew: fail, delete: fail }, new framework.ZLinkRuntimeMetrics(sink.provider));
      await assert.rejects(() => repository[method](...args), Failure);
      assertStoreOperations(sink, []);
    });
  }
}

test('Location input, codec, normal conflicts, expiry and recovery health do not count as backend errors', async () => {
  const sink = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(sink.provider);
  const { ZLinkLocationStoreRepository } = require('../../packages/framework/dist/runtime/locations/location-store-repository');
  const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
  const provider = new ZLinkInMemoryProviderLocationStore();
  const repository = new ZLinkLocationStoreRepository(provider, undefined, metrics);
  await repository.claimOwnerLease('owner', 15000);
  assert.equal((await repository.claimOwnerLease('owner', 15000)).kind, 'conflict');
  await assert.rejects(() => repository.listAuthorities('', undefined, 0), RangeError);
  const corrupt = new ZLinkLocationStoreRepository({
    async read() { return { kind: 'found', value: { bytes: Buffer.from('{'), version: { value: '1' }, storeNow: new Date() } }; },
    write: provider.write.bind(provider), scan: provider.scan.bind(provider)
  }, undefined, metrics);
  await assert.rejects(() => corrupt.readOwnerLease('owner'));
  const runtime = new framework.ZLinkLocationRuntime({ stores: { locationStore: repository, ownerLeaseStore: repository }, metrics });
  runtime.closeOwnerLeaseAdmission(new Error('recovery handler failed'));
  runtime.notifyIfStale({ status: framework.ZLinkLocationWriteStatus.RejectedConflict }, 'actor', 'key');
  assertStoreOperations(sink, []);
});


test('owner claim confirmation is a read and is counted once', async () => {
  const sink = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(sink.provider);
  const { ZLinkLocationStoreRepository } = require('../../packages/framework/dist/runtime/locations/location-store-repository');
  const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
  const provider = new ZLinkInMemoryProviderLocationStore();
  await new ZLinkLocationStoreRepository(provider).claimOwnerLease('owner', 15000);
  let reads = 0;
  const repository = new ZLinkLocationStoreRepository({
    read(key, signal) {
      if (++reads === 3) throw new Error('confirmation backend unavailable');
      return provider.read(key, signal);
    }, write: provider.write.bind(provider), scan: provider.scan.bind(provider)
  }, undefined, metrics);
  const runtime = new framework.ZLinkLocationRuntime({ stores: { locationStore: repository, ownerLeaseStore: repository }, metrics, ownerId: 'owner', setTimer: () => 0, clearTimer() {} });
  await runtime.start('node');
  assertStoreOperations(sink, ['read']);
});

test('shared Store label fixture rejects an undeclared operation', () => {
  const sink = { records: [{ name: 'zlink.location.store.errors', attributes: { operation: 'owner_lease_claim' } }] };
  assert.throws(() => assertStoreOperations(sink, ['owner_lease_claim']), assert.AssertionError);
});
