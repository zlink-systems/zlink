'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const connector = require('../../packages/stream-connector/dist');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');

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

test('RMETRIC-016 connector owns reconnect attempt counting', async () => {
  const { provider, records } = collector();
  let attempts = 0;
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:7999',
    meterProvider: provider,
    reconnect: { enabled: true, maxAttempts: 3, initialDelayMs: 1, maxDelayMs: 1 },
    transportFactory: {
      async connect() {
        attempts += 1;
        if (attempts < 3) throw new Error('retry');
        return { async write() {}, async close() {} };
      }
    }
  });
  await instance.connect();
  await instance.close();
  assert.equal(attempts, 3);
  assert.equal(records.filter((record) => record.name === 'zlink.stream.reconnects').length, 2);
});

test('RMETRIC-002 connector records handshake and frame bytes at the transport boundary', async () => {
  const { provider, records } = collector();
  const inbound = [];
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:7999',
    meterProvider: provider,
    dispatchMode: 'manual',
    transportFactory: {
      async connect() {
        return {
          async write(frame) { inbound.push(frame); },
          async read() { return inbound.shift(); },
          async close() {}
        };
      }
    }
  });

  await instance.connect();
  instance.send({ value: 'probe' }).packetName('MetricProbe').submit();
  await new Promise((resolve) => setImmediate(resolve));
  const outbound = records.find((record) => record.name === 'zlink.stream.outbound.bytes');
  await instance.dispatch();
  await instance.close();

  assert(records.some((record) => record.name === 'zlink.stream.handshake.duration'
    && record.attributes.transport === 'ws'));
  assert.equal(records.some((record) => record.name === 'zlink.stream.handshake.failures'), false);
  assert.equal(outbound.value > 0, true);
  assert.equal(outbound.attributes.transport, 'ws');
  assert.equal(records.find((record) => record.name === 'zlink.stream.inbound.bytes').value, outbound.value);
});

test('RMETRIC-003 connector records failed handshake with closed labels', async () => {
  const { provider, records } = collector();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'wss://127.0.0.1:7999',
    meterProvider: provider,
    transportFactory: { async connect() { throw new Error('tls failed'); } }
  });
  await assert.rejects(() => instance.connect());
  assert.deepEqual(records.find((record) => record.name === 'zlink.stream.handshake.failures'), {
    name: 'zlink.stream.handshake.failures',
    kind: 'counter',
    value: 1,
    attributes: { transport: 'wss', reason: 'transport_error' }
  });
});

test('RMETRIC-007 server catalog does not publish connector-only session bind metrics', async () => {
  const { provider, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  const socket = {
    send() { return true; },
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
