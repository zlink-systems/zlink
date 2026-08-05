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
          createObservableGauge(instrumentName, options) {
            instruments.push({ name: instrumentName, kind: 'gauge', unit: options?.unit });
            return { addCallback(callback) { observables.push({ name: instrumentName, callback }); } };
          }
        };
      }
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

test('RMETRIC-006 server runtime exposes the exact 42-instrument spec25 catalog', () => {
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
    ['zlink.relocation.duration', ['histogram', 's']],
    ['zlink.relocation.bytes', ['histogram', 'By']],
    ['zlink.relocation.interruption', ['histogram', 's']],
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
    ['zlink.host.shutdown.forced', ['counter', '{operation}']]
  ]);
  assert.equal(instruments.length, 42);
  assert.deepEqual(
    new Map(instruments.map(({ name, kind, unit }) => [name, [kind, unit]])),
    expected
  );
  assert(instruments.every(({ name }) => !name.startsWith('zlink.fanout.')));
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

test('RMETRIC-009 channel drops use normalized closed labels when tracing is off', async () => {
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

  assert.deepEqual(records.find((record) => record.name === 'zlink.mesh_node.messages.dropped'), {
    name: 'zlink.mesh_node.messages.dropped',
    kind: 'counter',
    value: 1,
    attributes: { surface: 'channel', message_kind: 'publish', reason: 'no_handler' }
  });
});

test('RMETRIC-015 bounded flow observer queue counts overflow even when tracing is off', async () => {
  const { provider, records } = collector();
  const metrics = new framework.ZLinkRuntimeMetrics(provider);
  let release;
  const blocked = new Promise((resolve) => { release = resolve; });
  class BlockingObserver {
    async onMessageFlow() { await blocked; }
  }
  const tracer = new framework.ZLinkMessageFlowTracer({
    diagnostics: { messageFlow: 'off', sampleRate: 1, includeMessageSizes: false },
    liveMode: { mode: 'off' },
    messageFlowObserverType: BlockingObserver
  }, { reportRuntimeTaskException() {} }, metrics, 1);
  const event = {
    outcome: 'received',
    surface: 'channel',
    messageKind: 'send',
    packetName: 'MetricProbe'
  };

  framework.flowIfEnabled(tracer, 'received').trace(event);
  await new Promise((resolve) => setImmediate(resolve));
  framework.flowIfEnabled(tracer, 'received').trace(event);
  framework.flowIfEnabled(tracer, 'received').trace(event);

  assert.equal(records.filter((record) => record.name === 'zlink.observability.events.overflow').length, 1);
  release();
  await new Promise((resolve) => setImmediate(resolve));
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
