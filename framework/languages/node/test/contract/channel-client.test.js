const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { once } = require('node:events');
const { fork } = require('node:child_process');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');

const telemetry = require('./helpers/telemetry-log-capture');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const { resolveFrameworkPacketName } = require('../../packages/framework/dist/runtime/messaging/packet-name');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const { RawServiceMeshRuntime } = require(
  '../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime'
);
const { SERVICE_WIRE_REQUIRED_CAPABILITY } = require(
  '../../packages/framework/dist/runtime/foundation/service-wire-constants.generated'
);
const { ZLinkNodeRawBindingPort } = require(
  '../../packages/framework/dist/runtime/backend/node/node-raw-binding-port'
);
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require(
  '../../packages/framework/dist/runtime/host/application-job-queue'
);
const serviceWire = require(
  '../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec'
);
const {
  keepChannelRequestAlive
} = require('../../packages/framework/dist/runtime/channels/channel-outbound-operations');

function dispatchOptions() {
  return framework.createFrameworkOptions((options) => {
    options.configureDispatch().messageFlow('errors');
  }).dispatch;
}
const frameworkProtobuf = require('../../packages/framework-codec-protobuf/dist/server/framework.cjs');
const nestjs = require('../../packages/nestjs/dist');
const { resolveModuleProviders } = require('./helpers/nestjs-test-utils');
const reservedPorts = new Set();

test.afterEach(async () => {
  // Native socket teardown becomes observable to the monitor drain asynchronously.
  await new Promise((resolve) => setTimeout(resolve, 0));
});

test('pending channel requests keep the Node event loop alive until they settle', async () => {
  const originalSetTimeout = global.setTimeout;
  const originalClearTimeout = global.clearTimeout;
  let keepalive;
  let cleared;
  let resolveReply;
  const reply = new Promise(resolve => {
    resolveReply = resolve;
  });
  global.setTimeout = (callback, delay) => {
    assert.equal(delay, 0x7fffffff);
    keepalive = { callback, delay, ref: true };
    return keepalive;
  };
  global.clearTimeout = timer => {
    cleared = timer;
  };
  try {
    const pending = keepChannelRequestAlive(reply);
    assert.equal(keepalive.ref, true);
    assert.equal(cleared, undefined);
    resolveReply('reply');
    assert.equal(await pending, 'reply');
    assert.equal(cleared, keepalive);
  } finally {
    global.setTimeout = originalSetTimeout;
    global.clearTimeout = originalClearTimeout;
  }
});

test('two in-process ClientServer nodes deliver a delayed reply to an awaited client request', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const serverRegistration = framework.createFrameworkRegistration({
    channels: {
      delayed: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'Ping',
          handler: {
            async handle(payload) {
              await new Promise(resolve => setTimeout(resolve, 25));
              return { value: payload.value };
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: { delayed: { client: { manualConnections: [endpoint] } } }
  });
  const server = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });
  const client = new framework.DefaultZLinkChannelClient(
    clientRegistration,
    clientRuntime.channelTransport
  );
  try {
    await server.start();
    await clientRuntime.start();
    const reply = await client.requestToChannel('delayed', typedPacket('Ping', { value: 'delayed' }))
      .timeout(1_000)
      .submit();
    assert.deepEqual(reply, { value: 'delayed' });
  } finally {
    await clientRuntime.stop();
    await server.stop();
  }
});

test('two Node RouteMesh nodes round-trip a channel request and retain the pending submit', async () => {
  const descriptor = (rid, endpoint, channels, role) => ({
    meshName: 'channel-round-trip', nodeRoutingId: rid, lifecycleGeneration: 1n,
    descriptorRevision: 1n, advertisedEndpoint: endpoint, channels, state: 'serving',
    securityIdentity: 'test', applicationVersion: 1n, protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: role, placementWeight: 100, activeCapacityLimit: 100,
    pendingCapacityLimit: 10, activeCapacityUsed: 0, pendingCapacityUsed: 0
  });
  const queue = () => new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const client = new RawServiceMeshRuntime({
    descriptor: descriptor('channel-client', 'tcp://127.0.0.1:0', [], 'client'),
    bindingPort: new ZLinkNodeRawBindingPort(), applicationJobQueue: queue()
  });
  const server = new RawServiceMeshRuntime({
    descriptor: descriptor('channel-server', 'tcp://127.0.0.1:0', [{ name: 'api', weight: 100 }], 'server'),
    bindingPort: new ZLinkNodeRawBindingPort(), applicationJobQueue: queue()
  });
  try {
    client.start();
    server.start();
    server.setServiceIngress(record => {
      if (record.command !== serviceWire.M6aServiceWireCommand.channelRequest) return undefined;
      const request = serviceWire.decodeChannelRequestHeader(record.parts[0]);
      server.replyService(record, [
        serviceWire.encodeReplyHeader(request.correlation),
        serviceWire.encodeApplicationPayload({
          packetName: 'Pong', contentType: 'application/json', payload: Buffer.from('{"ok":true}')
        })
      ]);
      return 'infrastructure';
    });
    client.connectPeer(server.topology.localDescriptor().advertisedEndpoint, server.topology.localDescriptor());
    server.connectPeer(client.topology.localDescriptor().advertisedEndpoint, client.topology.localDescriptor());
    //  turn 예산은 CPU 부하에 민감하다 — 한 turn이 하는 일이 줄어들면 같은 예산으로
    //  admission이 끝나지 않는다. 검증 대상은 준비가 되느냐이지 몇 turn에 되느냐가
    //  아니므로 예산을 넉넉히 둔다.
    for (let turn = 0; turn < 3000
      && (!client.isPeerRouteReady('channel-server') || !server.isPeerRouteReady('channel-client')); turn += 1) {
      await client.drainMonitorEvents(); await server.drainMonitorEvents();
      await client.announceExpectedPeers(); await server.announceExpectedPeers();
      await client.tickLiveness(); await server.tickLiveness();
      await client.pumpOne(); await server.pumpOne();
      await new Promise(resolve => setImmediate(resolve));
    }
    assert.equal(client.isPeerRouteReady('channel-server'), true);
    const pending = client.requestToChannel('api', {
      packetName: 'Ping', contentType: 'application/json', payload: Buffer.from('{"ping":true}')
    }, 1_000);
    for (let turn = 0; turn < 300; turn += 1) {
      await client.tickLiveness(); await server.tickLiveness();
      await client.pumpOne(); await server.pumpOne();
      await new Promise(resolve => setImmediate(resolve));
      if (await Promise.race([pending.promise.then(() => true), Promise.resolve(false)])) break;
    }
    const reply = await pending.promise;
    assert.equal(reply.terminalResult, 0);
    assert.equal(Buffer.from(reply.payload.payload).toString(), '{"ok":true}');
  } finally {
    client.close();
    server.close();
  }
});

test('raw RouteMesh applies current receive-flow state before bind and unregisters before close', async () => {
  const calls = [];
  const router = {
    setRoutingId() {},
    setReceiveFlowState(state) { calls.push(`flow:${state}`); },
    bind() { calls.push('bind'); },
    localEndpoint() { return 'tcp://127.0.0.1:9404'; },
    monitor() { return { drain() { return 0; }, statusReady() { return true; }, close() { calls.push('monitor:close'); } }; },
    close() { calls.push('router:close'); }
  };
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({
    maxQueuedApplicationJobs: 5n,
    pauseThresholdPercent: 80,
    resumeThresholdPercent: 40
  }, () => 1n));
  const permits = [];
  for (let index = 0; index < 4; index += 1) permits.push(await queue.acquire());
  const runtime = new RawServiceMeshRuntime({
    descriptor: {
      meshName: 'flow', nodeRoutingId: 'flow-node', lifecycleGeneration: 1n,
      descriptorRevision: 1n, advertisedEndpoint: 'tcp://127.0.0.1:9404', channels: [],
      state: 'preparing', securityIdentity: 'test', applicationVersion: 1n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
      placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 10,
      activeCapacityUsed: 0, pendingCapacityUsed: 0
    },
    bindingPort: {
      createHost() {
        return {
          createRouter() { return router; },
          close() { router.close(); }
        };
      }
    },
    applicationJobQueue: queue
  });
  runtime.start();
  assert.deepEqual(calls.slice(0, 2), ['flow:paused', 'bind']);
  permits.pop().releaseAfterInternalProcessing();
  permits.pop().releaseAfterInternalProcessing();
  assert.equal(calls.at(-1), 'flow:running');
  for (const permit of permits) permit.releaseAfterInternalProcessing();
  runtime.close();
  assert.deepEqual(calls.slice(-2), ['monitor:close', 'router:close']);
  const countAfterClose = calls.length;
  const afterClose = [];
  for (let index = 0; index < 4; index += 1) afterClose.push(await queue.acquire());
  assert.equal(calls.length, countAfterClose);
  for (const permit of afterClose) permit.releaseAfterInternalProcessing();
});

test('raw RouteMesh receive-flow controller preserves the latest reentrant transition', async () => {
  const calls = [];
  const permits = [];
  let releaseDuringPause = true;
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({
    maxQueuedApplicationJobs: 5n,
    pauseThresholdPercent: 80,
    resumeThresholdPercent: 40
  }, () => 1n));
  const router = {
    setRoutingId() {},
    setReceiveFlowState(state) {
      calls.push(`flow:${state}`);
      if (state === 'paused' && releaseDuringPause) {
        releaseDuringPause = false;
        permits[0].releaseAfterInternalProcessing();
        permits[1].releaseAfterInternalProcessing();
      }
    },
    bind() { calls.push('bind'); },
    localEndpoint() { return 'tcp://127.0.0.1:9405'; },
    monitor() { return { drain() { return 0; }, statusReady() { return true; }, close() {} }; },
    close() {}
  };
  const runtime = new RawServiceMeshRuntime({
    descriptor: {
      meshName: 'flow-race', nodeRoutingId: 'flow-race-node', lifecycleGeneration: 1n,
      descriptorRevision: 1n, advertisedEndpoint: 'tcp://127.0.0.1:9405', channels: [],
      state: 'preparing', securityIdentity: 'test', applicationVersion: 1n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
      placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 10,
      activeCapacityUsed: 0, pendingCapacityUsed: 0
    },
    bindingPort: {
      createHost() {
        return {
          createRouter() { return router; },
          close() { router.close(); }
        };
      }
    },
    applicationJobQueue: queue
  });

  runtime.start();
  for (let index = 0; index < 4; index += 1) permits.push(await queue.acquire());

  assert.equal(queue.pressureState(), 'running');
  assert.deepEqual(calls, [
    'flow:running',
    'bind',
    'flow:paused',
    'flow:running'
  ]);

  for (const permit of permits) permit.releaseAfterInternalProcessing();
  runtime.close();
});

test('raw RouteMesh reports a receive-flow configuration failure once', async () => {
  const expected = new Error('receive-flow configuration failed');
  const reported = [];
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({
    maxQueuedApplicationJobs: 5n,
    pauseThresholdPercent: 80,
    resumeThresholdPercent: 40
  }, () => 1n));
  const router = {
    setRoutingId() {},
    setReceiveFlowState(state) {
      if (state === 'paused') throw expected;
    },
    bind() {},
    localEndpoint() { return 'tcp://127.0.0.1:9406'; },
    monitor() { return { drain() { return 0; }, statusReady() { return true; }, close() {} }; },
    close() {}
  };
  const runtime = new RawServiceMeshRuntime({
    descriptor: {
      meshName: 'flow-failure', nodeRoutingId: 'flow-failure-node', lifecycleGeneration: 1n,
      descriptorRevision: 1n, advertisedEndpoint: 'tcp://127.0.0.1:9406', channels: [],
      state: 'preparing', securityIdentity: 'test', applicationVersion: 1n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
      placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 10,
      activeCapacityUsed: 0, pendingCapacityUsed: 0
    },
    bindingPort: {
      createHost() {
        return {
          createRouter() { return router; },
          close() { router.close(); }
        };
      }
    },
    applicationJobQueue: queue,
    onReceiveFlowConfigFailure(error) { reported.push(error); }
  });
  runtime.start();
  const permits = [];
  for (let index = 0; index < 4; index += 1) permits.push(await queue.acquire());

  assert.deepEqual(reported, [expected]);
  assert.equal(queue.snapshot().flowStateConfigFailureCount, 1n);

  for (const permit of permits) permit.releaseAfterInternalProcessing();
  runtime.close();
  assert.deepEqual(reported, [expected]);
});

test('raw RouteMesh startup fails when a paired backend omits receive-flow control', () => {
  const reported = [];
  const calls = [];
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({
    maxQueuedApplicationJobs: 5n,
    pauseThresholdPercent: 80,
    resumeThresholdPercent: 40
  }, () => 1n));
  const router = {
    setRoutingId() {},
    bind() { calls.push('bind'); },
    close() { calls.push('close'); }
  };
  const runtime = new RawServiceMeshRuntime({
    descriptor: {
      meshName: 'missing-flow', nodeRoutingId: 'missing-flow-node', lifecycleGeneration: 1n,
      descriptorRevision: 1n, advertisedEndpoint: 'tcp://127.0.0.1:9407', channels: [],
      state: 'preparing', securityIdentity: 'test', applicationVersion: 1n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
      placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 10,
      activeCapacityUsed: 0, pendingCapacityUsed: 0
    },
    bindingPort: {
      createHost() {
        return { createRouter() { return router; }, close() { router.close(); } };
      }
    },
    applicationJobQueue: queue,
    onReceiveFlowConfigFailure(error) { reported.push(error); }
  });

  assert.throws(() => runtime.start(), TypeError);
  assert.equal(queue.snapshot().flowStateConfigFailureCount, 1n);
  assert.equal(reported.length, 1);
  assert.deepEqual(calls, ['close']);
});

function typedPacket(packetName, value) {
  const PacketType = typeof value === 'string'
    ? { [packetName]: class extends String {} }[packetName]
    : { [packetName]: class {} }[packetName];
  return typeof value === 'string'
    ? new PacketType(value)
    : Object.assign(new PacketType(), value);
}

function meshChannelRegistration(meshName, channelName, options = {}) {
  return framework.createFrameworkRegistration({
    requestTimeoutMs: options.requestTimeoutMs,
    spotNodes: {
      [meshName]: {
        router: { bind: `inproc://contract-${meshName}` },
        requestTimeoutMs: options.meshRequestTimeoutMs,
        meshChannels: { [channelName]: { client: true } }
      }
    }
  });
}

test('typed packet identity is owned by the packet type and not by call or payload overrides', () => {
  class ConstructorPacket {}
  class DecoratedPacket {}
  class DecoratedSubclass extends DecoratedPacket {}
  framework.ZLinkPacket('ContractPacket')(DecoratedPacket);

  assert.equal(resolveFrameworkPacketName(new ConstructorPacket(), undefined, 'Channel'), 'ConstructorPacket');
  assert.equal(resolveFrameworkPacketName(new DecoratedPacket(), undefined, 'Channel'), 'ContractPacket');
  assert.equal(resolveFrameworkPacketName(new DecoratedSubclass(), undefined, 'Channel'), 'DecoratedSubclass');
  assert.throws(
    () => resolveFrameworkPacketName({ packetName: () => 'PayloadOverride' }, undefined, 'Channel'),
    framework.ZLinkConfigurationException
  );

  const registration = meshChannelRegistration('mesh', 'api');
  const call = new framework.DefaultZLinkRouteClient(registration, {
    async submitToChannel() {}, async requestToChannel() {}
  }).requestToChannel('api', new ConstructorPacket());
  assert.equal('packetName' in call, false);
});

test('Node-direct operations classify Object Client targets as NotFound', async () => {
  const node = {
    isObjectClientNodeDirectTarget() {
      return true;
    },
    sendToNode() {
      throw new Error('Object Client target must not reach transport submission.');
    },
    requestToNode() {
      throw new Error('Object Client target must not reach transport request.');
    }
  };
  const transport = new framework.ZLinkRuntimeRouteTransport(
    () => undefined,
    undefined,
    () => ({
      meshNode: () => node,
      meshCompletionTable: () => undefined
    })
  );
  const packet = typedPacket('NodeProbe', { value: 1 });

  assert.equal(
    (await transport.submit(
      'mesh',
      'client-node',
      undefined,
      packet
    )).status,
    ZLinkSubmitStatus.TargetNotFound
  );
  await assert.rejects(
    transport.request(
      'mesh',
      'client-node',
      undefined,
      packet,
      1000
    ),
    (error) =>
      error.kind === framework.ZLinkFrameworkErrorKind.NotFound
      && !('isRetriable' in error)
  );
});

function fakeSpotRouteBridge() {
  return {
    attachRouterChannel() {},
    send() { return { message() { return this; }, async submit() {} }; },
    request() { return { message() { return this; }, timeout() { return this; }, async submit() { return []; } }; },
    handleRouterReceived() { return false; },
    async dispose() {}
  };
}

test('ZLinkChannelClient rejects calls to channels without client capability', async () => {
  const client = new framework.DefaultZLinkChannelClient(framework.createFrameworkRegistration());

  await assert.rejects(
    () => client.sendToChannel('api', { ok: true }).submit(),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
});

test('ZLinkChannelClient reports NotConfigured when only the Server role exists', async () => {
  const client = new framework.DefaultZLinkChannelClient(framework.createFrameworkRegistration({
    channels: {
      api: {
        server: { bind: 'inproc://api' },
        sendHandlers: [{ packetName: 'Notice', handler: { handle() {} } }]
      }
    }
  }));

  for (const submit of [
    () => client.sendToChannel('api', { ok: true }).submit(),
    () => client.requestToChannel('api', { ok: true }).submit()
  ]) {
    await assert.rejects(
      submit,
      (error) => error instanceof framework.ZLinkFrameworkException
        && error.kind === framework.ZLinkFrameworkErrorKind.NotConfigured
    );
  }
});

test('ZLinkSendCall snapshots metadata and reports asynchronous admission once', async () => {
  const calls = [];
  const registration = meshChannelRegistration('mesh', 'api');
  const transport = {
    async submitToChannel(meshName, channelName, packetName, message, signal, metadata) {
      calls.push({ kind: 'async', meshName, channelName, packetName, message, signal, metadata: [...metadata] });
      return { status: ZLinkSubmitStatus.Submitted };
    },
    async requestToChannel() {},
    async submit() {},
    async request() {}
  };
  const client = new framework.DefaultZLinkRouteClient(registration, transport);
  const first = client.sendToChannel('api', typedPacket('Notice', { id: 1 }))
    .metadata('trace-id', 'one')
    .metadata(framework.zlinkMessageMetadata({
      'trace-id': 'two',
      tenant: 'blue'
    }));
  const second = client.sendToChannel('api', typedPacket('Notice', { id: 2 }))
    .metadata('tenant', 'green');

  assert.equal(await first.submit(), undefined);
  await assert.rejects(
    async () => first.submit(),
    /already been submitted/
  );
  assert.equal(await second.submit(), undefined);
  assert.deepEqual(calls.map(({ kind, meshName, channelName, metadata }) => ({
    kind,
    meshName,
    channelName,
    metadata
  })), [
    {
      kind: 'async',
      meshName: 'mesh',
      channelName: 'api',
      metadata: [['trace-id', 'two'], ['tenant', 'blue']]
    },
    {
      kind: 'async',
      meshName: 'mesh',
      channelName: 'api',
      metadata: [['tenant', 'green']]
    }
  ]);
});

test('application metadata exposes an immutable copied snapshot', () => {
  const source = new Map([['trace-id', 'one']]);
  const metadata = framework.zlinkMessageMetadata(source);
  source.set('trace-id', 'changed');
  source.set('tenant', 'blue');

  assert.deepEqual([...metadata.values], [['trace-id', 'one']]);
  assert.equal(metadata.find('trace-id'), 'one');
  assert.equal(metadata.values.set, undefined);
  assert.throws(() => metadata.values.set('trace-id', 'mutated'), TypeError);
  assert.deepEqual(
    [...framework.zlinkMessageMetadata(metadata.values).values],
    [['trace-id', 'one']]
  );
});

test('one-way clients reject when their registered runtime is not started', async () => {
  const registration = framework.createFrameworkRegistration({
    channels: {
      api: { client: { manualConnections: ['inproc://api'] } },
      events: { publisher: { bind: 'inproc://events' } }
    },
    routeChannels: [{ routerChannelId: 'route', manualConnections: ['inproc://route'] }]
  });

  await assert.rejects(
    () => new framework.DefaultZLinkChannelClient(registration).sendToChannel('api', typedPacket('Ping')).submit(),
    /runtime is not started/i
  );
  await assert.rejects(
    () => new framework.DefaultZLinkFanoutClient(registration).publish('events', typedPacket('Event')).submit(),
    /runtime is not started/i
  );
  await assert.rejects(
    () => new framework.DefaultZLinkRouteClient(registration).sendToNode('route', 'target', typedPacket('Ping')).submit(),
    /runtime is not started/i
  );
});

test('ZLinkFanoutClient exposes the current advertised publisher listener endpoint', () => {
  const registration = framework.createFrameworkRegistration({
    channels: {
      events: { publisher: { bind: 'tcp://127.0.0.1:0' } }
    }
  });
  const observedAt = new Date('2026-08-05T00:00:00.000Z');
  const fanout = new framework.DefaultZLinkFanoutClient(registration, {
    getFanoutListenerStatus(channelName) {
      assert.equal(channelName, 'events');
      return {
        channelName,
        endpoint: 'tcp://127.0.0.1:43127',
        observedAt
      };
    }
  });

  assert.deepEqual(fanout.getListenerStatus('events'), {
    channelName: 'events',
    endpoint: 'tcp://127.0.0.1:43127',
    observedAt
  });
});

test('ZLinkFanoutClient rejects listener status for a non-publisher channel', () => {
  const fanout = new framework.DefaultZLinkFanoutClient(framework.createFrameworkRegistration(), {
    getFanoutListenerStatus() {
      throw new Error('listener status must not reach transport');
    }
  });

  assert.throws(
    () => fanout.getListenerStatus('events'),
    (error) => error instanceof framework.ZLinkConfigurationException
      && /does not have a publisher capability/.test(error.message)
  );
});

test('ZLinkRouteClient one-way calls report asynchronous transport admission', async () => {
  const calls = [];
  const registration = framework.createFrameworkRegistration({
    routeChannels: [{ routerChannelId: 'route', manualConnections: ['inproc://route'] }]
  });
  const client = new framework.DefaultZLinkRouteClient(registration, {
    submit(routerChannelId, targetNodeRid, packetName, message) {
      calls.push({ routerChannelId, targetNodeRid, packetName, message });
    },
    async request() { return undefined; }
  });
  const packet = typedPacket('Ping', { value: 1 });

  const result = await client.sendToNode('route', 'target', packet).submit();

  assert.equal(result, undefined);
  assert.deepEqual(calls, [{
    routerChannelId: 'route',
    targetNodeRid: 'target',
    packetName: undefined,
    message: packet
  }]);
});

test('ZLinkRouteClient uses one SpotHandle snapshot and does not retry a stale request', async () => {
  const oldTarget = {
    meshName: 'play',
    nodeRid: 'node-old',
    spotId: 'spot-1',
    spotKind: framework.ZLinkSpotKind.User
  };
  const newTarget = { ...oldTarget, nodeRid: 'node-new' };
  let refreshCount = 0;
  const handle = framework.createSpotHandle('spot-1', oldTarget, async () => {
    refreshCount += 1;
    return newTarget;
  });
  const sends = [];
  const requests = [];
  const client = new framework.DefaultZLinkRouteClient(
    framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'play.route', manualConnections: ['inproc://play'] }]
    }),
    {
      submit() {},
      async request() {},
      async sendToSpot(target, message, options) {
        sends.push({ target, message, options });
      },
      async requestToSpot(target, request, options) {
        requests.push({ target, request, options });
        throw new framework.ZLinkFrameworkException(
          framework.ZLinkFrameworkErrorKind.NotFound,
          'stale spot route'
        );
      }
    },
    (meshName) => `${meshName}.route`
  );

  await client.sendToSpot(handle, typedPacket('Notice', { id: 1 })).submit();
  await assert.rejects(
    () => client
      .requestToSpot(handle, typedPacket('Lookup', { id: 2 }))
      .timeout(250)
      .submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );

  assert.equal(sends.length, 1);
  assert.equal(String(sends[0].target.targetNodeRid), 'node-old');
  assert.equal(sends[0].target.routerChannelId, 'play.route');
  assert.equal(sends[0].options.packetName, 'Notice');
  assert.deepEqual(requests.map((entry) => String(entry.target.targetNodeRid)), ['node-old']);
  assert.deepEqual(requests.map((entry) => entry.target.routerChannelId), ['play.route']);
  assert.deepEqual(requests.map((entry) => entry.options.timeoutMs), [250]);
  assert.equal(refreshCount, 0);
});

test('ZLinkRouteClient does not refresh or retry a Spot send rejected on a stale route', async () => {
  const oldTarget = {
    meshName: 'play',
    nodeRid: 'node-old',
    spotId: 'spot-1',
    spotKind: framework.ZLinkSpotKind.User
  };
  const newTarget = { ...oldTarget, nodeRid: 'node-new' };
  let refreshCount = 0;
  const handle = framework.createSpotHandle('spot-1', oldTarget, async () => {
    refreshCount += 1;
    return newTarget;
  });
  const targets = [];
  const client = new framework.DefaultZLinkRouteClient(
    framework.createFrameworkRegistration(),
    {
      submit() {},
      async request() {},
      async sendToSpot(target) {
        targets.push(String(target.targetNodeRid));
        return { status: ZLinkSubmitStatus.TargetNotFound };
      },
      async requestToSpot() {}
    }
  );

  const result = await client.sendToSpot(handle, typedPacket('Notice', { id: 1 })).submit();
  assert.equal(result, undefined);
  assert.deepEqual(targets, ['node-old']);
  assert.equal(refreshCount, 0);
});

test('ZLinkRouteClient does not refresh or retry an uncertain Spot request failure', async () => {
  const target = {
    meshName: 'play.route',
    nodeRid: 'node-a',
    spotId: 'spot-1',
    spotKind: framework.ZLinkSpotKind.User
  };
  let refreshCount = 0;
  let requestCount = 0;
  const handle = framework.createSpotHandle('spot-1', target, async () => {
    refreshCount += 1;
    return target;
  });
  const client = new framework.DefaultZLinkRouteClient(
    framework.createFrameworkRegistration(),
    {
      submit() {},
      async request() {},
      async sendToSpot() {},
      async requestToSpot() {
        requestCount += 1;
        throw new framework.ZLinkFrameworkException(
          framework.ZLinkFrameworkErrorKind.Unavailable,
          'delivery outcome is uncertain'
        );
      }
    }
  );

  await assert.rejects(
    () => client.requestToSpot(handle, typedPacket('Lookup', { id: 3 })).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  assert.equal(requestCount, 1);
  assert.equal(refreshCount, 0);
});

test('ZLinkRouteClient channel request call snapshots metadata and passes timeout to transport', async () => {
  const calls = [];
  const registration = meshChannelRegistration('mesh', 'api');
  const client = new framework.DefaultZLinkRouteClient(registration, {
    async submitToChannel() {},
    async requestToChannel(meshName, channelName, packetName, request, timeoutMs, _signal, metadata) {
      calls.push({ meshName, channelName, packetName, request, timeoutMs, metadata: [...metadata] });
      return { ok: true };
    },
    async submit() {},
    async request() {}
  });

  const reply = await client
    .requestToChannel('api', { id: 7 })
    .metadata('trace-id', 'one')
    .metadata(framework.zlinkMessageMetadata({ 'trace-id': 'two', tenant: 'blue' }))
    .timeout(250)
    .submit();

  assert.deepEqual(reply, { ok: true });
  assert.deepEqual(calls, [
    {
      meshName: 'mesh', channelName: 'api',
      packetName: undefined,
      request: { id: 7 },
      timeoutMs: 250,
      metadata: [['trace-id', 'two'], ['tenant', 'blue']]
    }
  ]);
});

test('ZLinkRouteClient applies registration request timeout when channel call timeout is omitted', async () => {
  const calls = [];
  const registration = meshChannelRegistration('mesh', 'api', { requestTimeoutMs: 7000 });
  const client = new framework.DefaultZLinkRouteClient(registration, {
    async submitToChannel() {},
    async requestToChannel(meshName, channelName, packetName, request, timeoutMs) {
      calls.push({ meshName, channelName, packetName, request, timeoutMs });
      return { ok: true };
    },
    async submit() {},
    async request() {}
  });

  const reply = await client
    .requestToChannel('api', { id: 7 })
    .submit();

  assert.deepEqual(reply, { ok: true });
  assert.deepEqual(calls, [
    { meshName: 'mesh', channelName: 'api', packetName: undefined, request: { id: 7 }, timeoutMs: 7000 }
  ]);
});

test('ZLinkRouteClient applies RouteMesh request timeout before registration default', async () => {
  const calls = [];
  const registration = meshChannelRegistration('mesh', 'api', {
    requestTimeoutMs: 7000,
    meshRequestTimeoutMs: 2000
  });
  const client = new framework.DefaultZLinkRouteClient(registration, {
    async submitToChannel() {},
    async requestToChannel(meshName, channelName, packetName, request, timeoutMs) {
      calls.push({ meshName, channelName, packetName, request, timeoutMs });
      return { ok: true };
    },
    async submit() {},
    async request() {}
  });

  const reply = await client
    .requestToChannel('api', { id: 7 })
    .submit();

  assert.deepEqual(reply, { ok: true });
  assert.deepEqual(calls, [
    { meshName: 'mesh', channelName: 'api', packetName: undefined, request: { id: 7 }, timeoutMs: 2000 }
  ]);
});

test('ZLinkRouteClient applies route channel request timeout before registration default', async () => {
  const calls = [];
  const registration = framework.createFrameworkRegistration({
    requestTimeoutMs: 7000,
    routeChannels: [
      {
        routerChannelId: 'route',
        requestTimeoutMs: 3000,
        manualConnections: ['inproc://route']
      }
    ]
  });
  const client = new framework.DefaultZLinkRouteClient(registration, {
    submit() {},
    async request(routerChannelId, targetNodeRid, packetName, request, timeoutMs) {
      calls.push({ routerChannelId, targetNodeRid, packetName, request, timeoutMs });
      return { ok: true };
    }
  });

  const reply = await client
    .requestToNode('route', 'target', { id: 7 })
    .submit();

  assert.deepEqual(reply, { ok: true });
  assert.deepEqual(calls, [
    {
      routerChannelId: 'route',
      targetNodeRid: 'target',
      packetName: undefined,
      request: { id: 7 },
      timeoutMs: 3000
    }
  ]);
  assert.equal('yield' in client.requestToNode('route', 'target', { id: 8 }), true);
});

test('route packet dispatcher sends channel envelopes to route handlers before Spot bridge fallback', async () => {
  let bridgeCalls = 0;
  const handledPayloads = [];
  const filterInvocations = [];
  const replyParts = [];
  const dispatcher = new framework.ZLinkRoutePacketDispatcher({
    routerChannelId: 'bingo.play',
    dispatchErrors: noDispatchErrorReporter(),
    filters: [{
      async invoke(invocation, next) {
        filterInvocations.push(invocation);
        return await next();
      }
    }],
    spotRouteBridge: {
      handleRouterReceived() {
        bridgeCalls += 1;
        return true;
      }
    },
    handlers: [{
      kind: 'request',
      packetName: 'AllocateBingoRoomReq',
      handler: {
        handle(payload, context) {
          handledPayloads.push({
            payload,
            meshName: context.meshName,
            sourceNodeRid: String(context.sourceNodeRid),
            correlationId: context.correlationId,
            hasRouterAlias: 'routerChannelId' in context
          });
          return { roomId: 'room-1' };
        }
      }
    }]
  });

  const parts = [
    zlink.Message.from(Buffer.from(JSON.stringify({
      formatMarker: 0xf2,
      flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
      flowOrigin: 1,
      kind: 1,
      channelName: 'bingo.play',
      messageName: 'AllocateBingoRoomReq',
      contentType: 'application/json',
      correlationId: 'route-corr',
      deadline: null,
      topic: null,
      errorCode: null,
      errorMessage: null
    }))),
    zlink.Message.from(Buffer.from(JSON.stringify({ mode: 'two-player' })))
  ];
  const replyToken = {};
  const router = {
    reply(routingId, token) {
      assert.equal(String(routingId), 'api-node');
      assert.equal(token, replyToken);
      return {
        message(message) {
          replyParts.push(message);
          return this;
        },
        submit() {}
      };
    }
  };

  try {
    await dispatcher.dispatch({
      parts,
      routingId: 'api-node',
      replyToken
    }, router);

    assert.equal(bridgeCalls, 0);
    assert.deepEqual(handledPayloads, [{
      payload: { mode: 'two-player' },
      meshName: 'bingo.play',
      sourceNodeRid: 'api-node',
      correlationId: 'route-corr',
      hasRouterAlias: false
    }]);
    assert.deepEqual(filterInvocations.map((context) => ({
      meshName: context.meshName,
      packetName: context.packetName,
      dispatchKind: context.dispatchKind,
      hasLegacyContext: 'context' in context,
      hasMessage: 'message' in context,
      hasSourceNodeRid: 'sourceNodeRid' in context
    })), [{
      meshName: 'bingo.play',
      packetName: 'AllocateBingoRoomReq',
      dispatchKind: framework.ZLinkHandlerDispatchKind.NodeDirectRequest,
      hasLegacyContext: false,
      hasMessage: false,
      hasSourceNodeRid: false
    }]);
    assert.equal(replyParts.length, 2);
  } finally {
    parts.forEach((part) => part.close());
    replyParts.forEach((part) => part.close?.());
  }
});

test('ZLinkChannelClient and fanout client reject pre-aborted submit before transport dispatch', async () => {
  const controller = new AbortController();
  controller.abort();
  const calls = [];
  const registration = framework.createFrameworkRegistration({
    channels: {
      api: { client: { manualConnections: ['inproc://api'] } },
      events: { publisher: { bind: 'inproc://events' } }
    }
  });
  const transport = {
    async send() {
      calls.push('send');
    },
    async request() {
      calls.push('request');
      return { ok: true };
    },
    async publish() {
      calls.push('publish');
    }
  };
  const client = new framework.DefaultZLinkChannelClient(registration, transport);
  const fanout = new framework.DefaultZLinkFanoutClient(registration, transport);

  await assertAborted(() => client.sendToChannel('api', 'hello').submit(controller.signal));
  await assertAborted(() => client.requestToChannel('api', typedPacket('Ping', 'ping')).submit(controller.signal));
  await assertAborted(() => fanout.publish('events', typedPacket('Event', 'event')).submit(controller.signal));
  assert.deepEqual(calls, []);
});

test('ZLinkFanoutClient preserves an explicitly supplied topic and derives packet name separately', async () => {
  const calls = [];
  const event = typedPacket('EventMsg', { value: 'payload' });
  const registration = framework.createFrameworkRegistration({
    channels: { events: { publisher: { bind: 'inproc://explicit-fanout-topic' } } }
  });
  const fanout = new framework.DefaultZLinkFanoutClient(registration, {
    async publish(channelName, topic, packetName, message, signal) {
      calls.push({ channelName, topic, packetName, message, signal });
      return { status: ZLinkSubmitStatus.Submitted };
    }
  });

  await fanout.publish('events', 'orders', event).submit();

  assert.equal(calls.length, 1);
  assert.equal(calls[0].channelName, 'events');
  assert.equal(calls[0].topic, 'orders');
  assert.equal(calls[0].packetName, 'EventMsg');
  assert.equal(calls[0].message, event);
});

test('one-way call validation wins over a pre-aborted signal', async () => {
  const controller = new AbortController();
  controller.abort();
  let transportAttempts = 0;
  const registration = framework.createFrameworkRegistration();
  const transport = {
    async submitToChannel() {
      transportAttempts += 1;
      return { status: ZLinkSubmitStatus.Submitted };
    },
    async publish() {
      transportAttempts += 1;
      return { status: ZLinkSubmitStatus.Submitted };
    }
  };

  const channel = new framework.DefaultZLinkChannelClient(registration, transport);
  const fanout = new framework.DefaultZLinkFanoutClient(registration, transport);

  await assert.rejects(
    () => channel.sendToChannel('missing-channel', 'hello').submit(controller.signal),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
  await assert.rejects(
    () => fanout.publish('missing-fanout', typedPacket('Event', 'event')).submit(controller.signal),
    framework.ZLinkConfigurationException
  );
  assert.equal(transportAttempts, 0);
});

test('Logical Multicast call object permits only one submit invocation', async () => {
  let attempts = 0;
  const registration = meshChannelRegistration('mesh', 'events');
  const client = new framework.DefaultZLinkSpotPublisherClient(registration, {
    async publish() {
      attempts += 1;
      return { status: ZLinkSubmitStatus.Submitted };
    }
  });
  const call = client.publish('mesh', 'events', 'topic', typedPacket('Event', 'event'));

  assert.equal(await call.submit(), undefined);
  await assert.rejects(() => call.submit(), (error) => {
    assert.equal(error instanceof framework.ZLinkFrameworkException, true);
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
    return true;
  });
  assert.equal(attempts, 1);
});

test('Logical Multicast completion does not expose target admission results', async () => {
  const registration = meshChannelRegistration('mesh', 'events');
  const client = new framework.DefaultZLinkSpotPublisherClient(registration, {
    async publish() {
      return { status: ZLinkSubmitStatus.Submitted };
    }
  });

  assert.equal(
    await client.publish('mesh', 'events', 'topic', typedPacket('Event', 'event')).submit(),
    undefined
  );
});

test('Logical Multicast pre-commit admission failure remains exceptional', async () => {
  const registration = meshChannelRegistration('mesh', 'events');
  const client = new framework.DefaultZLinkSpotPublisherClient(registration, {
    async publish() {
      return { status: ZLinkSubmitStatus.Backpressured };
    }
  });

  await assert.rejects(
    () => client.publish('mesh', 'events', 'topic', typedPacket('Event', 'event')).submit(),
    (error) => {
      assert.equal(error instanceof framework.ZLinkFrameworkException, true);
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      return true;
    }
  );
});

test('Logical Multicast call built before runtime disposal reports RuntimeShutdown on submit', async () => {
  const registration = meshChannelRegistration('mesh', 'events');
  let manager = {
    publish() {
      throw new Error('disposed runtime must not be called');
    }
  };
  const transport = new framework.ZLinkRuntimeSpotPublisherTransport(() => manager);
  const client = new framework.DefaultZLinkSpotPublisherClient(registration, transport);
  const call = client.publish(
    'mesh', 'events', 'topic', typedPacket('Event', 'event')
  );
  manager = undefined;

  await assert.rejects(call.submit(), (error) => {
    assert.equal(error instanceof framework.ZLinkFrameworkException, true);
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
    return true;
  });
});

test('ZLinkDealerChannelClientTransport rejects pre-aborted signal before creating socket operations', async () => {
  const controller = new AbortController();
  controller.abort();
  const calls = [];
  const transport = new framework.ZLinkDealerChannelClientTransport(
    {
      send() {
        calls.push('dealer.send');
        return createMultipartSubmitOperation();
      },
      request() {
        calls.push('dealer.request');
        return createMultipartRequestOperation();
      }
    },
    {
      publish() {
        calls.push('pub.publish');
        return createMultipartSubmitOperation();
      }
    }
  );

  await assertAborted(() => transport.send('api', 'Greeting', 'hello', controller.signal));
  await assertAborted(() => transport.request('api', 'Ping', 'ping', 250, controller.signal));
  await assertAborted(() => transport.publish('events', 'topic', 'Event', 'event', controller.signal));
  assert.deepEqual(calls, []);
});

test('ZLinkDealerChannelClientTransport maps native request connectivity failures to public route error', async () => {
  const nativeError = Object.assign(new Error('dealerRequest failed: Connection refused'), { code: 5 });
  const transport = new framework.ZLinkDealerChannelClientTransport({
    request() {
      return createMultipartRequestOperation({
        async submit() { throw nativeError; }
      });
    }
  });

  await assert.rejects(
    () => transport.request('api', 'Ping', 'ping', 100),
    (error) => error instanceof framework.ZLinkFrameworkException &&
      error.kind === framework.ZLinkFrameworkErrorKind.Unavailable &&
      !('isRetriable' in error) &&
      error.cause === nativeError
  );
});

test('ZLinkModule.forRoot provides concrete channel and fanout clients', () => {
  const builder = nestjs.zlinkFramework();
  builder.addRouteMesh('api').peerConnections().connect('inproc://api');
  const module = nestjs.ZLinkModule.forRoot(builder
    .addFanoutChannel('events')
      .enablePublisher('inproc://pub')
    .build());
  const channelProvider = module.providers.find((provider) => provider.provide === nestjs.ZLINK_CHANNEL_CLIENT);
  const fanoutProvider = module.providers.find((provider) => provider.provide === nestjs.ZLINK_FANOUT_CLIENT);

  assert.deepEqual(channelProvider.inject, [
    nestjs.ZLINK_FRAMEWORK_REGISTRATION,
    nestjs.ZLINK_FRAMEWORK_RUNTIME
  ]);
  assert.deepEqual(fanoutProvider.inject, [
    nestjs.ZLINK_FRAMEWORK_REGISTRATION,
    nestjs.ZLINK_FRAMEWORK_RUNTIME
  ]);
  assert.equal(typeof channelProvider.useFactory, 'function');
  assert.equal(typeof fanoutProvider.useFactory, 'function');
});

test('ZLinkChannelClient sends through public dealer/router binding sockets', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const endpoint = `inproc://framework-channel-send-${process.pid}-${Date.now()}`;

  try {
    router.bind(endpoint);
    dealer.connect(endpoint);

    const registration = framework.createFrameworkRegistration({
      channels: { api: { client: { manualConnections: [endpoint] } } }
    });
    const client = new framework.DefaultZLinkChannelClient(
      registration,
      new framework.ZLinkDealerChannelClientTransport(dealer)
    );

    const submit = client
      .sendToChannel('api', typedPacket('Greeting', 'hello'))
      .metadata('trace-id', 'send-1')
      .submit();
    assert.equal(await submit, undefined);

    const received = new zlink.Received();
    try {
      assert.equal(router.recv(received), true);
      const envelope = decodeDotnetEnvelope(received.parts);
      assert.equal(envelope.header.kind, 3);
      assert.equal(envelope.header.channelName, 'api');
      assert.equal(envelope.header.messageName, 'Greeting');
      assert.deepEqual(envelope.header.metadata, { 'trace-id': 'send-1' });
      assert.equal(envelope.body, 'hello');
    } finally {
      received.close();
    }
  } finally {
    dealer.close();
    router.close();
    ctx.close();
  }
});

test('ZLinkChannelClient request/reply round-trips through public binding sockets', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  let routerMonitor;
  let dealerMonitor;

  try {
    routerMonitor = router.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    dealerMonitor = dealer.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    router.bind(endpoint);
    dealer.connect(endpoint);
    await waitForMonitorConnectionReady(routerMonitor, 'channel client router connection', router);
    await waitForMonitorConnectionReady(dealerMonitor, 'channel client dealer connection', router);
    routerMonitor.close();
    routerMonitor = null;
    dealerMonitor.close();
    dealerMonitor = null;

    const registration = framework.createFrameworkRegistration({
      channels: { api: { client: { manualConnections: [endpoint] } } }
    });
    const client = new framework.DefaultZLinkChannelClient(
      registration,
      new framework.ZLinkDealerChannelClientTransport(dealer)
    );

    const replyPromise = client.requestToChannel('api', typedPacket('Ping', { value: 'ping' })).timeout(1000).submit();
    const request = await recvRouterMessage(router);
    const envelope = decodeDotnetEnvelope(request.parts);
    assert.equal(envelope.header.kind, 1);
    assert.equal(envelope.header.channelName, 'api');
    assert.equal(envelope.header.messageName, 'Ping');
    assert.deepEqual(envelope.body, { value: 'ping' });
    assert.notEqual(request.replyToken, null);
    submitRawReplyMultipart(
      router.reply(request.routingId, request.replyToken),
      encodeDotnetEnvelope({
        ...envelope.header,
        kind: 2,
        deadline: null,
        topic: null,
        errorCode: null,
        errorMessage: null
      }, { value: 'pong' })
    );

    const reply = await withTimeout(replyPromise, 1000, 'framework channel request reply');
    assert.deepEqual(reply, { value: 'pong' });
    request.close();
  } finally {
    dealerMonitor?.close();
    routerMonitor?.close();
    dealer.close();
    router.close();
    ctx.close();
  }
});

test('route raw SPOT requests await independent binding request promises', async () => {
  let active = 0;
  let maxActive = 0;
  const calls = [];
  const releases = [];
  const fakeSpot = {
    requestToSpot(targetNodeRid, targetSpot, request) {
      active += 1;
      maxActive = Math.max(maxActive, active);
      calls.push({
        targetNodeRid,
        spotId: targetSpot,
        request: request.data().toString()
      });
      const sequence = calls.length;
      return new Promise((resolve) => releases.push(() => {
        active -= 1;
        resolve([zlink.Message.from(Buffer.from(`reply-${sequence}`))]);
      }));
    }
  };
  const registration = framework.createFrameworkRegistration({
    routeChannels: [{ routerChannelId: 'room.route' }],
    spotNodes: {
      play: {
        router: { bind: 'inproc://play', routingId: 'play-node' }
      }
    }
  });
  const manager = new framework.ZLinkChannelRuntimeManager(
    registration,
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([
    ['play', {
      entrySpot() {
        return fakeSpot;
      }
    }]
  ]));
  const remoteAddress = {
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-node',
    spotKind: framework.ZLinkSpotKind.Entry
  };

  const first = manager.routeRequestRawToSpot(
    remoteAddress,
    zlink.Message.from(Buffer.from('first')),
    1000
  );
  const second = manager.routeRequestRawToSpot(
    remoteAddress,
    zlink.Message.from(Buffer.from('second')),
    1000
  );

  await waitUntil(() => calls.length === 2);
  assert.equal(active, 2);
  assert.equal(calls[0].request, 'first');
  releases.shift()();
  assert.equal(active, 1);
  assert.equal(calls[1].request, 'second');
  releases.shift()();

  const [firstReply, secondReply] = await Promise.all([first, second]);
  assert.equal(firstReply[0].data().toString(), 'reply-1');
  assert.equal(secondReply[0].data().toString(), 'reply-2');
  firstReply[0].close();
  secondReply[0].close();
  assert.equal(maxActive, 2);
});

test('aborted raw SPOT request closes its late binding reply', async () => {
  const calls = [];
  const releases = [];
  const fakeSpot = {
    requestToSpot(_targetNodeRid, _targetSpot, request) {
      calls.push(request.data().toString());
      return new Promise((resolve) => releases.push(resolve));
    }
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'room.route' }],
      spotNodes: { play: { router: { bind: 'inproc://play', routingId: 'play-node' } } }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([['play', { entrySpot() { return fakeSpot; } }]]));
  const address = {
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-node',
    spotKind: framework.ZLinkSpotKind.Entry
  };
  const abort = new AbortController();
  const first = manager.routeRequestRawToSpot(
    address,
    zlink.Message.from(Buffer.from('first')),
    1000,
    abort.signal
  );
  const second = manager.routeRequestRawToSpot(
    address,
    zlink.Message.from(Buffer.from('second')),
    1000
  );

  await waitUntil(() => calls.length === 2);
  abort.abort();
  await assert.rejects(first, /aborted/i);
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(calls, ['first', 'second']);

  let lateReplyClosed = 0;
  const lateReply = { close() { lateReplyClosed++; } };
  releases.shift()([lateReply]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(lateReplyClosed, 1);
  releases.shift()([zlink.Message.from(Buffer.from('second-reply'))]);
  const secondReply = await second;
  assert.equal(secondReply[0].data().toString(), 'second-reply');
  secondReply[0].close();
});

test('raw SPOT request timeout is delegated to the binding request promise', async () => {
  const calls = [];
  const releases = [];
  const fakeSpot = {
    requestToSpot(_targetNodeRid, _targetSpot, request) {
      calls.push(request.data().toString());
      return new Promise((resolve) => releases.push(resolve));
    }
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      requestTimeoutMs: 20,
      routeChannels: [{ routerChannelId: 'room.route' }],
      spotNodes: { play: { router: { bind: 'inproc://play', routingId: 'play-node' } } }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([['play', { entrySpot() { return fakeSpot; } }]]));
  const address = {
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-node',
    spotKind: framework.ZLinkSpotKind.Entry
  };
  const first = manager.routeRequestRawToSpot(address, zlink.Message.from(Buffer.from('first')));
  const second = manager.routeRequestRawToSpot(address, zlink.Message.from(Buffer.from('second')), 1000);

  await waitUntil(() => calls.length === 2);
  releases.shift()([zlink.Message.from(Buffer.from('first-reply'))]);
  releases.shift()([zlink.Message.from(Buffer.from('second-reply'))]);
  const replies = await Promise.all([first, second]);
  replies.flat().forEach(part => part.close());
});

test('SpotNode router is not classified as packet route channel', () => {
  const registration = framework.createFrameworkRegistration({
    routeChannels: [{ routerChannelId: 'play-node' }],
    spotNodes: {
      'play-node': {
        router: { bind: 'inproc://play-node', routingId: 'play-node' }
      }
    }
  });
  const manager = new framework.ZLinkChannelRuntimeManager(
    registration,
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([
    ['play-node', {
      entrySpot() {
        return { routingId: 'play-node' };
      }
    }]
  ]));

  assert.equal(manager.canRouteChannel('play-node'), true);
  assert.equal(manager.canRoutePacketChannel('play-node'), false);
});

test('route raw SPOT request awaits the binding request once', async () => {
  let attempts = 0;
  const fakeSpot = {
    async requestToSpot() {
      attempts += 1;
      return [zlink.Message.from(Buffer.from('ready-reply'))];
    }
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      requestTimeoutMs: 1000,
      routeChannels: [{ routerChannelId: 'room.route' }],
      spotNodes: {
        play: {
          router: { bind: 'inproc://play', routingId: 'play-node' }
        }
      }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([
    ['play', {
      entrySpot() {
        return fakeSpot;
      }
    }]
  ]));

  const reply = await manager.routeRequestRawToSpot(
    {
      routerChannelId: 'room.route',
      targetNodeRid: 'session-node',
      spotId: 'session-node',
      spotKind: framework.ZLinkSpotKind.Entry
    },
    zlink.Message.from(Buffer.from('request')),
    1000
  );

  assert.equal(attempts, 1);
  assert.equal(reply[0].data().toString(), 'ready-reply');
  reply[0].close();
});

test('route raw SPOT request from a user spot awaits the binding request once', async () => {
  let attempts = 0;
  const sourceSpot = {
    async requestToSpot() {
      attempts += 1;
      return [zlink.Message.from(Buffer.from('replacement-ready'))];
    }
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({ requestTimeoutMs: 1000 }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );

  const reply = await manager.routeRequestRawFromSpotToSpot(
    sourceSpot,
    {
      routerChannelId: 'room.route',
      targetNodeRid: 'replacement-node',
      spotId: 'replacement-room',
      spotKind: framework.ZLinkSpotKind.User
    },
    zlink.Message.from(Buffer.from('request')),
    1000
  );

  assert.equal(attempts, 1);
  assert.equal(reply[0].data().toString(), 'replacement-ready');
  reply[0].close();
});

test('route raw SPOT request aborts its wait without resubmitting', async () => {
  let attempts = 0;
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      requestTimeoutMs: 1000,
      routeChannels: [{ routerChannelId: 'room.route' }],
      spotNodes: {
        play: { router: { bind: 'inproc://play', routingId: 'play-node' } }
      }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([['play', {
    entrySpot() {
      return {
        requestToSpot() {
          attempts += 1;
          return new Promise(() => undefined);
        }
      };
    }
  }]]));
  const abort = new AbortController();
  const request = zlink.Message.from(Buffer.from('request'));
  const pending = manager.routeRequestRawToSpot({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-node',
    spotKind: framework.ZLinkSpotKind.Entry
  }, request, 1000, abort.signal);

  await waitUntil(() => attempts > 0);
  abort.abort();
  await assert.rejects(pending, /The operation was aborted/);
  const attemptsAtAbort = attempts;
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(attempts, attemptsAtAbort);
  request.close();
});

test('source Spot raw request aborts after native submission and closes a late reply', async () => {
  let complete;
  const sourceSpot = {
    requestToSpot() {
      return new Promise((resolve) => { complete = resolve; });
    }
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration(),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer() }),
    fakeContext()
  );
  const abort = new AbortController();
  const request = zlink.Message.from(Buffer.from('request'));
  const pending = manager.routeRequestRawFromSpotToSpot(sourceSpot, {
    routerChannelId: 'room.route',
    targetNodeRid: 'play-node',
    spotId: 'room-1'
  }, request, 1000, abort.signal);

  abort.abort();
  await assert.rejects(pending, /The operation was aborted/);
  let closeCount = 0;
  const lateReply = { close() { closeCount += 1; } };
  complete([lateReply]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(closeCount, 1);
  request.close();
});

test('route channel request propagates binding route rejection', async () => {
  const router = fakeRouteRouter();
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{
        routerChannelId: 'room.route',
        bind: 'inproc://session-route',
        routingId: 'session-node'
      }]
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router }),
    fakeContext()
  );

  await assert.rejects(
    () => manager.routeRequest('room.route', 'play-node', 'EnsurePlayerActorReq', { actorId: 'player-1' }, 100),
    /Route channel is not connected/
  );
  assert.equal(router.requestAttempts, 1);
});

test('route channel request delegates its timeout to the binding Promise', async () => {
  const router = fakeRouteRouter({ acceptWithoutReply: true });
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{
        routerChannelId: 'room.route',
        bind: 'inproc://session-route',
        routingId: 'session-node'
      }]
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router }),
    fakeContext()
  );

  await assert.rejects(
    manager.routeRequest(
      'room.route', 'play-node', 'EnsurePlayerActorReq', { actorId: 'player-1' }, 30
    ),
    /binding request timed out/
  );
  assert.equal(router.requestAttempts, 1);
});

test('route channel with SPOT acceptance starts one route receive loop for shared router frames', async () => {
  const router = fakeRouteRouter();
  const taskNames = [];
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{
        routerChannelId: 'room.route',
        bind: 'inproc://play-route',
        routingId: 'play-node'
      }],
      spotNodes: {
        play: {
          router: { bind: 'inproc://play-spot', routingId: 'play-node' }
        }
      }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router }),
    fakeContext()
  );
  const spotNode = {
    createRouteBridge() {
      return fakeSpotRouteBridge();
    },
    entrySpot() {
      return {
        requestToSpot() {
          throw new Error('spot request not used');
        }
      };
    }
  };
  manager.setSpotNodes(new Map([['play', spotNode]]));

  manager.start({
    errorSink: { reportRuntimeTaskException() {} },
    run(name) {
      taskNames.push(name);
      return Promise.resolve();
    }
  });

  assert.deepEqual(taskNames, ['route:room.route']);
  await manager.dispose();
});

test('route bridge raw request awaits its binding Promise reply', async () => {
  const router = fakeRouteRouter();
  const bridge = {
    attachRouterChannel() {},
    request() {
      return {
        message() {
          return this;
        },
        timeout() {
          return this;
        },
        async submit() {
          return [zlink.Message.from(Buffer.from(JSON.stringify({ ok: true, response: { value: 'reply' } })))];
        }
      };
    },
    handleRouterReceived() {
      return false;
    },
    async dispose() {}
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{
        routerChannelId: 'room.route',
        bind: 'inproc://play-route',
        routingId: 'play-node'
      }],
      spotNodes: {
        play: {
          router: { bind: 'inproc://play-spot', routingId: 'play-node' }
        }
      }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([['play', {
    createRouteBridge() {
      return bridge;
    },
    entrySpot() {
      return {
        requestToSpot() {
          throw new Error('spot request not used');
        }
      };
    }
  }]]));

  let stopLoop = false;
  manager.start({
    errorSink: { reportRuntimeTaskException() {} },
    run(_name, task) {
      void task({
        get aborted() {
          return stopLoop;
        }
      });
      return Promise.resolve();
    }
  });

  const request = manager.routeRequestRawToSpot({
    routerChannelId: 'room.route',
    targetNodeRid: 'play-node',
    spotId: 'room-1'
  }, zlink.Message.from(Buffer.from('request')), 100);
  await new Promise((resolve) => setImmediate(resolve));
  const reply = await request;
  assert.deepEqual(JSON.parse(reply[0].data().toString()), { ok: true, response: { value: 'reply' } });
  reply[0].close();
  stopLoop = true;
  await manager.dispose();
});

test('channel runtime disposal does not add a Framework request owner around the binding Promise', async () => {
  const bridge = {
    attachRouterChannel() {},
    request() {
      return {
        message() { return this; },
        timeout() { return this; },
        submit() { return new Promise(() => undefined); }
      };
    },
    handleRouterReceived() { return false; },
    async dispose() {}
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'room.route' }],
      spotNodes: { play: { router: { bind: 'inproc://play', routingId: 'play-node' } } }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router: fakeRouteRouter() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([['play', {
    createRouteBridge() { return bridge; },
    entrySpot() { return { requestToSpot() { return false; } }; }
  }]]));
  manager.start({
    errorSink: { reportRuntimeTaskException() {} },
    run() { return Promise.resolve(); }
  });
  const request = zlink.Message.from(Buffer.from('request'));
  const pending = manager.routeRequestRawToSpot({
    routerChannelId: 'room.route',
    targetNodeRid: 'play-node',
    spotId: 'room-1'
  }, request, undefined);

  await new Promise((resolve) => setImmediate(resolve));
  await manager.dispose();
  void pending.catch(() => undefined);
  request.close();
});

test('route bridge sends await each binding Promise directly', async () => {
  const router = fakeRouteRouter();
  const submittedTargets = [];
  const bridge = {
    attachRouterChannel() {},
    send(_channelName, targetNodeRid) {
      return {
        message() {
          return this;
        },
        flags() {
          return this;
        },
        async submit() {
          submittedTargets.push(String(targetNodeRid));
        }
      };
    },
    request() {
      throw new Error('request not used');
    },
    handleRouterReceived() {
      return false;
    },
    async dispose() {}
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{
        routerChannelId: 'room.route',
        bind: 'inproc://play-route',
        routingId: 'play-node'
      }],
      spotNodes: {
        play: {
          router: { bind: 'inproc://play-spot', routingId: 'play-node' }
        }
      }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([['play', {
    createRouteBridge() {
      return bridge;
    },
    entrySpot() {
      return {
        requestToSpot() {
          throw new Error('spot request not used');
        }
      };
    }
  }]]));

  manager.start({
    errorSink: { reportRuntimeTaskException() {} },
    run() {
      return Promise.resolve();
    }
  });

  const first = manager.routeSendToSpot({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node-a',
    spotId: 'session-node-a'
  }, 'Notify', { value: 1 });
  const second = manager.routeSendToSpot({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node-b',
    spotId: 'session-node-b'
  }, 'Notify', { value: 2 });

  await Promise.all([first, second]);

  assert.deepEqual(submittedTargets, ['session-node-a', 'session-node-b']);
  await manager.dispose();
});

test('route raw SPOT request uses route bridge before SpotNode router fallback', async () => {
  const router = fakeRouteRouter();
  const bridgeCalls = [];
  let spotNodeRouterUsed = false;
  const bridge = {
    attachRouterChannel(channelName, socket, options) {
      bridgeCalls.push({ kind: 'attach', channelName, socket, options });
    },
    request(channelName, targetNodeRid, spotId) {
      const call = { kind: 'request', channelName, targetNodeRid, spotId };
      bridgeCalls.push(call);
      return {
        message(message) {
          call.message = message.data().toString();
          return this;
        },
        timeout(timeoutMs) {
          call.timeoutMs = timeoutMs;
          return this;
        },
        async submit() {
          call.submitted = true;
          return [zlink.Message.from(Buffer.from('bridge-reply'))];
        }
      };
    },
    handleRouterReceived() {
      return false;
    },
    async dispose() {}
  };
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{
        routerChannelId: 'room.route',
        bind: 'inproc://play-route',
        routingId: 'play-node'
      }],
      spotNodes: {
        play: {
          router: { bind: 'inproc://play-spot', routingId: 'play-node' }
        }
      }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router }),
    fakeContext()
  );
  const spotNode = {
    routingId: 'play-node',
    createRouteBridge() {
      return bridge;
    },
    entrySpot() {
      return {
        requestToSpot() {
          spotNodeRouterUsed = true;
          throw new Error('SpotNode router fallback must not be used when bridge is available');
        }
      };
    }
  };
  manager.setSpotNodes(new Map([['play', spotNode]]));

  manager.start({
    errorSink: { reportRuntimeTaskException() {} },
    run() {
      return Promise.resolve();
    }
  });
  const reply = await manager.routeRequestRawToSpot(
    {
      routerChannelId: 'room.route',
      targetNodeRid: 'session-node',
      spotId: 'session-node',
      spotKind: framework.ZLinkSpotKind.Entry
    },
    zlink.Message.from(Buffer.from('bridge-request')),
    700
  );

  assert.equal(spotNodeRouterUsed, false);
  assert.equal(router.requestAttempts, 0);
  assert.equal(reply[0].data().toString(), 'bridge-reply');
  reply[0].close();
  assert.equal(bridgeCalls[0].kind, 'attach');
  assert.equal(bridgeCalls[0].channelName, 'room.route');
  assert.deepEqual(bridgeCalls[1], {
    kind: 'request',
    channelName: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-node',
    message: 'bridge-request',
    timeoutMs: 700,
    submitted: true
  });
  await manager.dispose();
});

test('route raw SPOT request prefers the named Spot mesh when route and Spot mesh names match', async () => {
  let spotRequests = 0;
  const manager = new framework.ZLinkChannelRuntimeManager(
    framework.createFrameworkRegistration({
      routeChannels: [{
        routerChannelId: 'delivery-couriers',
        bind: 'inproc://courier-route',
        routingId: 'courier-node-1'
      }],
      spotNodes: {
        'delivery-couriers': {
          router: { bind: 'inproc://courier-spot', routingId: 'courier-node-1' }
        }
      }
    }),
    fakeChannelAdapter({ dealer: fakeBackpressuredDealer(), router: fakeRouteRouter() }),
    fakeContext()
  );
  manager.setSpotNodes(new Map([['delivery-couriers', {
    routingId: 'courier-node-1',
    createRouteBridge() {
      return {
        ...fakeSpotRouteBridge(),
        request() {
          throw new Error('same-name actor/session traffic must not use the external route bridge');
        }
      };
    },
    entrySpot() {
      return {
        async requestToSpot() {
          spotRequests += 1;
          return [zlink.Message.from(Buffer.from('spot-reply'))];
        }
      };
    }
  }]]));
  manager.start({
    errorSink: { reportRuntimeTaskException() {} },
    run() { return Promise.resolve(); }
  });

  const request = zlink.Message.from(Buffer.from('spot-request'));
  const reply = await manager.routeRequestRawToSpot({
    routerChannelId: 'delivery-couriers',
    targetNodeRid: 'courier-session',
    spotId: 'courier-session',
    spotKind: framework.ZLinkSpotKind.Entry
  }, request, 700);

  assert.equal(spotRequests, 1);
  assert.equal(reply[0].data().toString(), 'spot-reply');
  reply[0].close();
  request.close();
  await manager.dispose();
});

test('ZLinkModule channel client uses runtime host channel transport after bootstrap', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const serverRegistration = framework.createFrameworkRegistration({
    channels: {
      api: {
        server: { bind: endpoint, routingId: 'nestjs-transport-server' },
        requestHandlers: [{
          packetName: 'Ping',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, 'api');
              assert.equal(context.packetName, 'Ping');
              assert.deepEqual(payload, { value: 'ping' });
              return { value: 'pong' };
            }
          }
        }]
      }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({
    registration: serverRegistration
  });
  const builder = nestjs.zlinkFramework();
  builder.addClientServerChannel('api').client().connect(endpoint);
  const module = nestjs.ZLinkModule.forRoot(builder.build());
  const container = await resolveModuleProviders(module, [
    nestjs.ZLINK_FRAMEWORK_RUNTIME,
    nestjs.ZLINK_CHANNEL_CLIENT
  ]);
  const runtime = container.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
  const client = container.get(nestjs.ZLINK_CHANNEL_CLIENT);

  try {
    await serverRuntime.start();
    await runtime.start();

    const reply = await client
      .requestToChannel('api', typedPacket('Ping', { value: 'ping' }))
      .timeout(1000)
      .submit();
    assert.deepEqual(reply, { value: 'pong' });
  } finally {
    await runtime.stop();
    await serverRuntime.stop();
  }
});

test('CH-001 ZLinkFrameworkRuntimeHost dispatches client-server channel request handlers', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const calls = [];
  const serverRegistration = framework.createFrameworkRegistration({
    channels: {
      play: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'CreateGame',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, 'play');
              assert.equal(context.packetName, 'CreateGame');
              const reply = { created: payload.gameName };
              calls.push(reply);
              return reply;
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      play: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);
    await waitUntil(() => clientRuntime.clientServerRuntime.isReady('play'), 5000);
    const reply = await client
      .requestToChannel('play', typedPacket('CreateGame', { gameName: 'sample' }))
      .timeout(1000)
      .submit();

    assert.deepEqual(calls, [{ created: 'sample' }]);
    assert.deepEqual(reply, { created: 'sample' });
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('ZLinkFrameworkRuntimeHost waits for in-flight channel dispatch before closing router', async () => {
  const calls = [];
  let releaseHandler;
  const handlerStarted = new Promise((resolve) => {
    releaseHandler = resolve;
  });
  let handlerCanFinish;
  const handlerRelease = new Promise((resolve) => {
    handlerCanFinish = resolve;
  });
  const router = fakeRuntimeRouter(calls, {
    parts: encodeDotnetEnvelope({
      kind: 1,
      channelName: 'api',
      messageName: 'SlowReq',
      contentType: 'application/json',
      correlationId: 'slow-1',
      deadline: null,
      topic: null,
      errorCode: null,
      errorMessage: null
    }, { value: 'wait' }).map(fakeMessagePart),
    routingId: 'client-node',
    replyToken: {},
    close() {
      calls.push('received:close');
      this.parts.forEach((part) => part.close());
    }
  });
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      channels: {
        api: {
          server: { bind: 'inproc://slow-api' },
          requestHandlers: [{
            packetName: 'SlowReq',
            handler: {
              async handle(payload) {
                calls.push(`handler:start:${payload.value}`);
                releaseHandler();
                await handlerRelease;
                calls.push('handler:finish');
                return { ok: true };
              }
            }
          }]
        }
      }
    })
  }, {
    backendAdapterFactory: fakeRuntimeBackendAdapterFactory(calls, router)
  });

  await runtime.start();
  await handlerStarted;
  const stop = runtime.stop();
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(calls.includes('router:dispose'), false);
  handlerCanFinish();
  await stop;
  assert.deepEqual(calls.filter((call) => call === 'router:reply' || call === 'router:dispose'), [
    'router:reply',
    'router:dispose'
  ]);
});

test('ZLinkFrameworkRuntimeHost applies server socket maxMessageSize', async () => {
  const calls = [];
  const router = fakeRuntimeRouter(calls);
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      channels: {
        api: {
          server: {
            bind: 'inproc://max-message-size-api',
            maxMessageSize: 2048
          },
          requestHandlers: [{
            packetName: 'Ping',
            handler: {
              handle() {
                return { ok: true };
              }
            }
          }]
        }
      }
    })
  }, {
    backendAdapterFactory: fakeRuntimeBackendAdapterFactory(calls, router)
  });

  try {
    await runtime.start();
    assert.equal(router.maxMessageSize, 2048);
  } finally {
    await runtime.stop();
  }
});

test('CH-006 ZLinkFrameworkRuntimeHost dispatches client-server send handlers', async () => {
  const channelName = 'play-send';
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const calls = [];
  const serverRegistration = framework.createFrameworkRegistration({
    channels: {
      [channelName]: {
        server: { bind: endpoint },
        sendHandlers: [{
          packetName: 'RecordCommand',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, channelName);
              assert.equal(context.packetName, 'RecordCommand');
              calls.push(payload.gameName);
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      [channelName]: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);
    await submitWhenReachable(() =>
      client.sendToChannel(channelName, typedPacket('RecordCommand', { gameName: 'sample' })).submit()
    );

    await waitFor(() => calls.length === 1, 'CH-006 channel send handler evidence');
    assert.deepEqual(calls, ['sample']);
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('DERR-001 ZLinkFrameworkRuntimeHost replies error and reports provider record for missing channel request handler', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  telemetry.reset();
  const dispatchEvents = telemetry.records
  const serverRegistration = framework.createFrameworkRegistration({
    dispatch: dispatchOptions(),
    channels: {
      play: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'KnownReq',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, 'play');
              assert.equal(context.packetName, 'KnownReq');
              return { value: `known:${payload.value}` };
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      play: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);

    // Runtime topology is the readiness contract. Waiting for it avoids
    // spending the whole request deadline in the first not-yet-connected
    // submission when coverage instrumentation delays socket monitoring.
    await waitUntil(() => clientRuntime.clientServerRuntime.isReady('play'));
    const knownBefore = await client
      .requestToChannel('play', typedPacket('KnownReq', { value: 'before' }))
      .timeout(1000)
      .submit();
    assert.deepEqual(knownBefore, { value: 'known:before' });

    await assert.rejects(
      () => client.requestToChannel('play', typedPacket('UnknownReq', { value: 'missing' })).timeout(1000).submit(),
      /No channel request handler is registered for 'play:UnknownReq'/
    );

    await waitUntil(() => dispatchEvents.length === 1, 1000);
    assert.equal(dispatchEvents[0].surface, 'channel');
    assert.equal(dispatchEvents[0].messageKind, 'request');
    assert.equal(dispatchEvents[0].outcome, 'failed');
    assert.equal(dispatchEvents[0].reason, 'no_handler');
    assert.equal(dispatchEvents[0].action, 'reply_error');
    assert.equal(dispatchEvents[0].packetName, 'UnknownReq');
    assert.equal(dispatchEvents[0].channelName, 'play');

    const knownAfter = await client
      .requestToChannel('play', typedPacket('KnownReq', { value: 'after' }))
      .timeout(1000)
      .submit();
    assert.deepEqual(knownAfter, { value: 'known:after' });
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('DERR-002 ZLinkFrameworkRuntimeHost reports provider record for missing channel send handler', async () => {
  const channelName = 'play-missing-send';
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  telemetry.reset();
  const dispatchEvents = telemetry.records
  const serverRegistration = framework.createFrameworkRegistration({
    dispatch: dispatchOptions(),
    channels: {
      [channelName]: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'KnownReq',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, channelName);
              assert.equal(context.packetName, 'KnownReq');
              return { value: `known:${payload.value}` };
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      [channelName]: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);

    const knownBefore = await submitWhenReachable(() =>
      client.requestToChannel(channelName, typedPacket('KnownReq', { value: 'before-send-error' })).timeout(1000).submit()
    );
    assert.deepEqual(knownBefore, { value: 'known:before-send-error' });

    client.sendToChannel(channelName, typedPacket('UnknownCommand', { value: 'missing-send' })).submit();

    await waitUntil(() => dispatchEvents.length === 1, 1000);
    assert.equal(dispatchEvents[0].surface, 'channel');
    assert.equal(dispatchEvents[0].messageKind, 'send');
    assert.equal(dispatchEvents[0].outcome, 'failed');
    assert.equal(dispatchEvents[0].reason, 'no_handler');
    assert.equal(dispatchEvents[0].action, 'drop');
    assert.equal(dispatchEvents[0].packetName, 'UnknownCommand');
    assert.equal(dispatchEvents[0].channelName, channelName);

    const knownAfter = await client
      .requestToChannel(channelName, typedPacket('KnownReq', { value: 'after-send-error' }))
      .timeout(1000)
      .submit();
    assert.deepEqual(knownAfter, { value: 'known:after-send-error' });
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('REG-003 ZLinkFrameworkRuntimeHost dispatches manual channel handlers and reports missing packets', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const fanoutEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  telemetry.reset();
  const dispatchEvents = telemetry.records;
  const sendCalls = [];
  const publishCalls = [];
  const serverRegistration = framework.createFrameworkRegistration({
    dispatch: dispatchOptions(),
    channels: {
      'manual-reg': {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'ManualRegisteredReq',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, 'manual-reg');
              assert.equal(context.packetName, 'ManualRegisteredReq');
              return { value: `manual:${payload.value}` };
            }
          }
        }],
        sendHandlers: [{
          packetName: 'ManualRegisteredCommand',
          handler: {
            handle(payload, context) {
              sendCalls.push({
                payload,
                channelName: context.channelName,
                packetName: context.packetName
              });
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      'manual-reg': { client: { manualConnections: [endpoint] } }
    }
  });
  const publisherRegistration = framework.createFrameworkRegistration({
    channels: {
      'manual-events': { publisher: { bind: fanoutEndpoint } }
    }
  });
  const subscriberRegistration = framework.createFrameworkRegistration({
    dispatch: dispatchOptions(),
    channels: {
      'manual-events': {
        subscriber: { manualConnections: [fanoutEndpoint] },
        publishHandlers: [{
          packetName: 'ManualRegisteredEvent',
          handler: {
            handle(payload, context) {
              publishCalls.push({
                payload,
                channelName: context.channelName,
                packetName: context.packetName,
                topic: context.topic
              });
            }
          }
        }]
      }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });
  const publisherRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: publisherRegistration });
  const subscriberRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: subscriberRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    await publisherRuntime.start();
    await subscriberRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);
    const fanout = new framework.DefaultZLinkFanoutClient(publisherRegistration, publisherRuntime.channelTransport);

    const reply = await submitWhenReachable(() =>
      client
        .requestToChannel('manual-reg', typedPacket('ManualRegisteredReq', { value: 'registered' }))
        .timeout(1000)
        .submit()
    );
    assert.deepEqual(reply, { value: 'manual:registered' });

    await client
      .sendToChannel('manual-reg', typedPacket('ManualRegisteredCommand', { value: 'command' }))
      .submit();
    await waitFor(() => sendCalls.length === 1, 'REG-003 manual send handler');
    assert.deepEqual(sendCalls, [{
      payload: { value: 'command' },
      channelName: 'manual-reg',
      packetName: 'ManualRegisteredCommand'
    }]);

    await publishUntilHandled(
      fanout,
      'manual-events',
      'manual',
      'ManualRegisteredEvent',
      { value: 'published' },
      () => publishCalls.length === 1
    );
    assert.deepEqual(publishCalls, [{
      payload: { value: 'published' },
      channelName: 'manual-events',
      packetName: 'ManualRegisteredEvent',
      topic: 'ManualRegisteredEvent'
    }]);

    await assert.rejects(
      () => client
        .requestToChannel('manual-reg', typedPacket('ManualMissingReq', { value: 'missing' }))
        .timeout(1000)
        .submit(),
      (error) => error instanceof framework.ZLinkFrameworkException
        && error.kind === framework.ZLinkFrameworkErrorKind.NotFound
        && /No channel request handler is registered for 'manual-reg:ManualMissingReq'/.test(error.message)
    );

    await client
      .sendToChannel('manual-reg', typedPacket('ManualMissingCommand', { value: 'missing-command' }))
      .submit();

    await publishUntilHandled(
      fanout,
      'manual-events',
      'manual',
      'ManualMissingEvent',
      { value: 'missing-event' },
      () => hasDispatchEvent(
        dispatchEvents,
        'send',
        'no_handler',
        'drop',
        'ManualMissingEvent',
        'manual-events',
        'classic_fanout'
      )
    );

    await waitUntil(() =>
      hasDispatchEvent(
        dispatchEvents,
        'request',
        'no_handler',
        'reply_error',
        'ManualMissingReq',
        'manual-reg'
      ) &&
      hasDispatchEvent(
        dispatchEvents,
        'send',
        'no_handler',
        'drop',
        'ManualMissingCommand',
        'manual-reg'
      ) &&
      hasDispatchEvent(
        dispatchEvents,
        'send',
        'no_handler',
        'drop',
        'ManualMissingEvent',
        'manual-events',
        'classic_fanout'
      ),
    1000);
  } finally {
    await subscriberRuntime.stop();
    await publisherRuntime.stop();
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('DERR-007 ZLinkFrameworkRuntimeHost replies error and reports provider record for handler exception', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  telemetry.reset();
  const dispatchEvents = telemetry.records
  const serverRegistration = framework.createFrameworkRegistration({
    dispatch: dispatchOptions(),
    channels: {
      play: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'KnownReq',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, 'play');
              assert.equal(context.packetName, 'KnownReq');
              return { value: `known:${payload.value}` };
            }
          }
        }, {
          packetName: 'ThrowReq',
          handler: {
            handle() {
              throw new Error('DERR-007 handler exception');
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      play: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);

    const knownBefore = await submitWhenReachable(() =>
      client.requestToChannel('play', typedPacket('KnownReq', { value: 'before-handler-error' })).timeout(1000).submit()
    );
    assert.deepEqual(knownBefore, { value: 'known:before-handler-error' });

    await assert.rejects(
      () => client.requestToChannel('play', typedPacket('ThrowReq', { value: 'boom' })).timeout(1000).submit(),
      /DERR-007 handler exception/
    );

    await waitUntil(() => dispatchEvents.length === 1, 1000);
    assert.equal(dispatchEvents[0].surface, 'channel');
    assert.equal(dispatchEvents[0].messageKind, 'request');
    assert.equal(dispatchEvents[0].outcome, 'failed');
    assert.equal(dispatchEvents[0].reason, 'handler_exception');
    assert.equal(dispatchEvents[0].action, 'reply_error');
    assert.equal(dispatchEvents[0].packetName, 'ThrowReq');
    assert.equal(dispatchEvents[0].channelName, 'play');
    assert.equal(dispatchEvents[0].errorType, 'Error');
    assert.equal(dispatchEvents[0].errorMessage, 'DERR-007 handler exception');

    const knownAfter = await client
      .requestToChannel('play', typedPacket('KnownReq', { value: 'after-handler-error' }))
      .timeout(1000)
      .submit();
    assert.deepEqual(knownAfter, { value: 'known:after-handler-error' });
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('CH-002 manual endpoint round-robin distributes requests across three servers', async () => {
  const endpoints = [
    `tcp://127.0.0.1:${await reservePort()}`,
    `tcp://127.0.0.1:${await reservePort()}`,
    `tcp://127.0.0.1:${await reservePort()}`
  ];
  const servers = [
    createRoundRobinServer(endpoints[0], 'server-a'),
    createRoundRobinServer(endpoints[1], 'server-b'),
    createRoundRobinServer(endpoints[2], 'server-c')
  ];
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      'round-robin': { client: { manualConnections: endpoints } }
    }
  });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    for (const server of servers) {
      await server.runtime.start();
    }
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);
    const seenServers = new Set();
    for (let i = 0; i < 30 && seenServers.size < 3; i++) {
      const reply = await submitWhenReachable(() =>
        client
          .requestToChannel('round-robin', typedPacket('RoundRobinProbe', { requestId: `warmup-${i}` }))
          .timeout(1000)
          .submit()
      );
      seenServers.add(reply.serverId);
    }
    assert.deepEqual([...seenServers].sort(), ['server-a', 'server-b', 'server-c']);

    const replies = [];
    for (let i = 0; i < 90; i++) {
      replies.push(await client
        .requestToChannel('round-robin', typedPacket('RoundRobinProbe', { requestId: `verify-${i}` }))
        .timeout(1000)
        .submit());
    }

    assertRoundRobinCounts(countBy(replies.map((reply) => reply.serverId)));
    assertRoundRobinCounts(Object.fromEntries(servers.map((server) => [
      server.serverId,
      server.evidence.filter((requestId) => requestId.startsWith('verify-')).length
    ])));
  } finally {
    await clientRuntime.stop();
    for (const server of [...servers].reverse()) {
      await server.runtime.stop();
    }
  }
});

test('DSC-008 requestToChannel traffic survives location scale-out and scale-in', async () => {
  const locationProvider = new framework.ZLinkInMemoryProviderLocationStore();
  const locationQuery = new framework.ZLinkLocationStoreRepository(locationProvider);
  const heldPorts = await reserveHeldPorts(3);
  const providerAEndpoint = `tcp://127.0.0.1:${heldPorts.ports[0]}`;
  const providerBEndpoint = `tcp://127.0.0.1:${heldPorts.ports[1]}`;
  const providerCEndpoint = `tcp://127.0.0.1:${heldPorts.ports[2]}`;
  const providerA = createScaleoutProvider(locationProvider, providerAEndpoint, 'provider-a');
  const providerB = createScaleoutProvider(locationProvider, providerBEndpoint, 'provider-b');
  const providerC = createScaleoutProvider(locationProvider, providerCEndpoint, 'provider-c');
  let clientAppA;
  let clientAppB;

  try {
    await heldPorts.release(providerAEndpoint);
    await providerA.runtime.start();
    await waitForReadyEndpoints(locationQuery, [providerAEndpoint]);
    clientAppA = await createScaleoutClientApp(locationProvider);
    clientAppB = await createScaleoutClientApp(locationProvider);
    const clientA = clientAppA.get(nestjs.ZLINK_CHANNEL_CLIENT);
    const clientB = clientAppB.get(nestjs.ZLINK_CHANNEL_CLIENT);

    const first = await requestScaleoutProbe(clientA, 'node-warmup-a');
    assert.equal(first.providerId, 'provider-a');
    assert.equal(providerA.evidence.includes('node-warmup-a'), true);

    const singleProviderTraffic = await runScaleoutTrafficBatch([clientA, clientB], 'node-traffic-a', 10);
    for (const providerId of singleProviderTraffic.observedProviders) {
      assert.equal(providerId, 'provider-a');
    }

    await heldPorts.release(providerBEndpoint);
    await providerB.runtime.start();
    await waitForReadyEndpoints(locationQuery, [providerAEndpoint, providerBEndpoint]);
    await heldPorts.release(providerCEndpoint);
    await providerC.runtime.start();
    await waitForReadyEndpoints(locationQuery, [providerAEndpoint, providerBEndpoint, providerCEndpoint]);
    const scaleoutTraffic = await waitForScaleoutTrafficProviders([clientA, clientB], 'node-scaleout', ['provider-b', 'provider-c']);
    assertRequestIdsHandledOnce(scaleoutTraffic.completedRequestIds, providerA, providerB, providerC);

    await providerA.runtime.stop();
    await waitUntilEndpointIsNotReady(locationQuery, providerAEndpoint);
    await waitForAutoConnectPollCycle();

    const providerACountBeforeScaleIn = providerA.evidence.length;
    const scaleinTraffic = await runScaleoutTrafficBatch([clientA, clientB], 'node-scalein-traffic', 10);
    assertRequestIdsHandledOnce(scaleinTraffic.completedRequestIds, providerA, providerB, providerC);
    for (const providerId of scaleinTraffic.observedProviders) {
      assert.notEqual(providerId, 'provider-a');
    }
    const scaleinRequestIds = [];
    const scaleinProviders = new Set();
    for (let i = 0; i < 10; i += 1) {
      const requestId = `node-scalein-${i}`;
      scaleinRequestIds.push(requestId);
      const client = i % 2 === 0 ? clientA : clientB;
      const reply = await requestScaleoutProbe(client, requestId);
      scaleinProviders.add(reply.providerId);
      assert.notEqual(reply.providerId, 'provider-a');
    }
    assert.equal(providerA.evidence.length, providerACountBeforeScaleIn);
    for (const providerId of scaleinProviders) {
      assert.equal(providerId === 'provider-b' || providerId === 'provider-c', true);
    }
    assertRequestIdsHandledOnce(scaleinRequestIds, providerA, providerB, providerC);
  } finally {
    await heldPorts.releaseAll();
    await clientAppB?.close();
    await clientAppA?.close();
    await Promise.allSettled([
      providerC.runtime.stop(),
      providerB.runtime.stop(),
      providerA.runtime.stop()
    ]);
  }
});

test('DSC-009 same routing id different endpoint replaces located provider', async () => {
  const locationStore = new framework.ZLinkInMemoryLocationStore();
  const providerV1Endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const providerV2Endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const providerRid = 'api-a';

  const providerV1Lease = await locationStore.claimOwnerLease('provider-v1', 30000);
  assert.equal(providerV1Lease.kind, 'claimed');
  await locationStore.updateClientServer(
    scaleoutPeer(providerV1Lease.token, providerRid, providerV1Endpoint),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  await waitForSingleReadyEndpoint(locationStore, providerRid, providerV1Endpoint);

  await locationStore.removeAllByOwner(providerV1Lease.token);
  await locationStore.releaseOwnerLease(providerV1Lease.token);
  const providerV2Lease = await locationStore.claimOwnerLease('provider-v2', 30000);
  assert.equal(providerV2Lease.kind, 'claimed');
  await locationStore.updateClientServer(
    scaleoutPeer(providerV2Lease.token, providerRid, providerV2Endpoint),
    framework.ZLinkLocationWriteIntent.NewClaim
  );

  await waitForSingleReadyEndpoint(locationStore, providerRid, providerV2Endpoint);
});

test('ZLinkFrameworkRuntimeHost uses channel serializer registry for typed request replies', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const contentType = 'application/x-test-codec';
  const calls = [];
  const serializer = {
    serialize(value) {
      return zlink.Message.from(Buffer.from(JSON.stringify({ wrapped: value })));
    },
    deserialize(message) {
      return JSON.parse(Buffer.from(message.data()).toString()).wrapped;
    }
  };
  const registrationOptions = {
    codecs: {
      serializers: [{ contentType, serializer }]
    }
  };
  const serverRegistration = framework.createFrameworkRegistration({
    ...registrationOptions,
    channels: {
      play: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'CreateGame',
          handler: {
            handle(payload, context) {
              assert.equal(context.contentType, contentType);
              assert.equal(Buffer.isBuffer(payload), false);
              assert.deepEqual(payload, { gameName: 'sample' });
              const reply = { created: payload.gameName };
              calls.push(reply);
              return reply;
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    ...registrationOptions,
    channels: {
      play: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);
    const reply = await submitWhenReachable(() =>
      client.requestToChannel('play', typedPacket('CreateGame', { gameName: 'sample' })).timeout(1000).submit()
    );

    assert.deepEqual(calls, [{ created: 'sample' }]);
    assert.deepEqual(reply, { created: 'sample' });
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('CDC-001 ZLinkFrameworkRuntimeHost JSON codec round-trips nested arrays and nullable fields', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const request = {
    name: 'root',
    revision: 42,
    optionalLabel: null,
    tags: ['alpha', 'beta'],
    children: [
      { name: 'first', rank: 1 },
      { name: 'second', rank: 2 }
    ],
    nested: { name: 'nested', rank: 3 }
  };
  const serverRegistration = framework.createFrameworkRegistration({
    channels: {
      codec: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'JsonCodecProbe',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, 'codec');
              assert.equal(context.packetName, 'JsonCodecProbe');
              assert.deepEqual(payload, request);
              return payload;
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    channels: {
      codec: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);
    const reply = await submitWhenReachable(() =>
      client.requestToChannel('codec', typedPacket('JsonCodecProbe', request)).timeout(1000).submit()
    );

    assert.deepEqual(reply, request);
    assert.equal(reply.optionalLabel, null);
    assert.deepEqual(reply.tags, ['alpha', 'beta']);
    assert.deepEqual(reply.nested, { name: 'nested', rank: 3 });
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('ZLinkFrameworkRuntimeHost uses protobuf codec extension for channels', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const contentType = 'application/x-protobuf';
  const calls = [];
  const codecs = new framework.DefaultZLinkCodecRegistryBuilder()
    .use(frameworkProtobuf.zlinkProtobufCodec());
  const registrationOptions = {
    codecs: {
      codecs: codecs.registeredCodecs,
      serializers: [...codecs.registeredSerializers].map(([registeredContentType, serializer]) => ({
        contentType: registeredContentType,
        serializer
      }))
    }
  };
  const serverRegistration = framework.createFrameworkRegistration({
    ...registrationOptions,
    channels: {
      play: {
        server: { bind: endpoint },
        requestHandlers: [{
          packetName: 'CreateGame',
          handler: {
            handle(payload, context) {
              assert.equal(context.contentType, contentType);
              calls.push(payload);
              return { created: payload.gameName, players: payload.players };
            }
          }
        }]
      }
    }
  });
  const clientRegistration = framework.createFrameworkRegistration({
    ...registrationOptions,
    channels: {
      play: { client: { manualConnections: [endpoint] } }
    }
  });
  const serverRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: serverRegistration });
  const clientRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: clientRegistration });

  try {
    await serverRuntime.start();
    await clientRuntime.start();
    const client = new framework.DefaultZLinkChannelClient(clientRegistration, clientRuntime.channelTransport);
    const reply = await submitWhenReachable(() =>
      client.requestToChannel('play', typedPacket('CreateGame', { gameName: 'sample', players: ['p1', 'p2'] }))
        .timeout(1000)
        .submit()
    );

    assert.deepEqual(reply, { created: 'sample', players: ['p1', 'p2'] });
    assert.deepEqual(calls, [{ gameName: 'sample', players: ['p1', 'p2'] }]);
  } finally {
    await clientRuntime.stop();
    await serverRuntime.stop();
  }
});

test('PUB-001 ZLinkFrameworkRuntimeHost delivers the same sequence to three fanout subscribers', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const topic = 'ProfileChanged';
  const first = [];
  const second = [];
  const third = [];
  const publisherRegistration = framework.createFrameworkRegistration({
    channels: {
      events: { publisher: { bind: endpoint } }
    }
  });
  const createSubscriberRegistration = (calls) => framework.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: [endpoint] },
        publishHandlers: [{
          packetName: 'ProfileChanged',
          handler: {
            handle(payload, context) {
              calls.push({
                payload,
                channelName: context.channelName,
                packetName: context.packetName,
                topic: context.topic,
                contentType: context.contentType
              });
            }
          }
        }]
      }
    }
  });
  const publisherRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: publisherRegistration });
  const firstRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: createSubscriberRegistration(first) });
  const secondRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: createSubscriberRegistration(second) });
  const thirdRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: createSubscriberRegistration(third) });

  try {
    await publisherRuntime.start();
    await firstRuntime.start();
    await secondRuntime.start();
    await thirdRuntime.start();

    const fanout = new framework.DefaultZLinkFanoutClient(publisherRegistration, publisherRuntime.channelTransport);
    const common = await publishUntilCommonFanoutSequence(fanout, topic, first, second, third);
    for (const calls of [first, second, third]) {
      assert.ok(calls.some((call) =>
        call.payload.sequence === common &&
        call.channelName === 'events' &&
        call.packetName === 'ProfileChanged' &&
        call.topic === topic &&
        call.contentType === 'application/json'
      ));
    }
  } finally {
    await thirdRuntime.stop();
    await secondRuntime.stop();
    await firstRuntime.stop();
    await publisherRuntime.stop();
  }
});

test('fanout publisher binds during runtime start before the first publish', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const calls = [];
  const publisherRegistration = framework.createFrameworkRegistration({
    channels: {
      events: { publisher: { bind: endpoint } }
    }
  });
  const subscriberRegistration = framework.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: [endpoint] },
        publishHandlers: [{
          packetName: 'ProfileChanged',
          handler: {
            handle(payload) {
              calls.push(payload);
            }
          }
        }]
      }
    }
  });
  const publisherRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: publisherRegistration });
  const subscriberRuntime = new framework.ZLinkFrameworkRuntimeHost({ registration: subscriberRegistration });

  try {
    await publisherRuntime.start();
    await subscriberRuntime.start();
    await new Promise((resolve) => setTimeout(resolve, 200));

    const fanout = new framework.DefaultZLinkFanoutClient(publisherRegistration, publisherRuntime.channelTransport);
    await fanout
      .publish('events', typedPacket('ProfileChanged', { sequence: 1 }))
      .submit();

    await waitFor(() => calls.length === 1, 'first fanout publish after runtime start');
    assert.deepEqual(calls[0], { sequence: 1 });
  } finally {
    await subscriberRuntime.stop();
    await publisherRuntime.stop();
  }
});

test('fanout subscriberConnections changes the live manual receive set', async () => {
  const firstEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const secondEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const calls = [];
  const firstPublisherRegistration = framework.createFrameworkRegistration({
    channels: { events: { publisher: { bind: firstEndpoint } } }
  });
  const secondPublisherRegistration = framework.createFrameworkRegistration({
    channels: { events: { publisher: { bind: secondEndpoint } } }
  });

  let subscriberConnections;
  const subscriberOptions = framework.createFrameworkOptions((builder) => {
    const channel = builder.addFanoutChannel('events');
    channel.connect(firstEndpoint);
    subscriberConnections = channel.subscriberConnections();
  });
  subscriberOptions.channels.events.publishHandlers = [{
    packetName: 'ProfileChanged',
    handler: {
      handle(payload) {
        calls.push(payload);
      }
    }
  }];
  const subscriberRegistration = framework.createFrameworkRegistration(subscriberOptions);
  const firstPublisherRuntime = new framework.ZLinkFrameworkRuntimeHost({
    registration: firstPublisherRegistration
  });
  const secondPublisherRuntime = new framework.ZLinkFrameworkRuntimeHost({
    registration: secondPublisherRegistration
  });
  const subscriberRuntime = new framework.ZLinkFrameworkRuntimeHost({
    registration: subscriberRegistration
  });

  try {
    await firstPublisherRuntime.start();
    await subscriberRuntime.start();
    await secondPublisherRuntime.start();
    const firstFanout = new framework.DefaultZLinkFanoutClient(
      firstPublisherRegistration,
      firstPublisherRuntime.channelTransport
    );
    const secondFanout = new framework.DefaultZLinkFanoutClient(
      secondPublisherRegistration,
      secondPublisherRuntime.channelTransport
    );

    await publishUntilHandled(
      firstFanout,
      'events',
      'ProfileChanged',
      'ProfileChanged',
      { source: 'first' },
      () => calls.some((payload) => payload.source === 'first')
    );

    subscriberConnections.connect(secondEndpoint);
    assert.deepEqual(subscriberConnections.listConnections(), [firstEndpoint, secondEndpoint]);
    await publishUntilHandled(
      secondFanout,
      'events',
      'ProfileChanged',
      'ProfileChanged',
      { source: 'second-before-disconnect' },
      () => calls.some((payload) => payload.source === 'second-before-disconnect')
    );

    subscriberConnections.disconnect(secondEndpoint);
    assert.deepEqual(subscriberConnections.listConnections(), [firstEndpoint]);
    const beforeDisconnect = calls.length;
    for (let attempt = 0; attempt < 3; attempt += 1) {
      await secondFanout
        .publish('events', typedPacket('ProfileChanged', { source: 'second-after-disconnect' }))
        .submit();
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    assert.equal(calls.length, beforeDisconnect);
  } finally {
    await secondPublisherRuntime.stop();
    await subscriberRuntime.stop();
    await firstPublisherRuntime.stop();
  }
});

test('fanout builder and subscriber handle share the manual endpoint set', () => {
  const firstEndpoint = 'tcp://127.0.0.1:9511';
  const secondEndpoint = 'tcp://127.0.0.1:9512';
  let subscriberConnections;
  const options = framework.createFrameworkOptions((builder) => {
    const channel = builder.addFanoutChannel('events');
    subscriberConnections = channel.subscriberConnections();
    channel.connect(firstEndpoint);
    subscriberConnections.connect(secondEndpoint);
    channel.connect(secondEndpoint);
  });

  assert.deepEqual(
    subscriberConnections.listConnections(),
    [firstEndpoint, secondEndpoint]
  );
  assert.deepEqual(
    options.channels.events.subscriber.manualConnections,
    [firstEndpoint, secondEndpoint]
  );
});

test('ZLinkModule route client uses runtime host route transport after bootstrap', async () => {
  const localEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const remoteEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const builder = nestjs.zlinkFramework();
  const mesh = builder
    .addRouteMesh('mesh')
      .listen(localEndpoint)
      .routingId('node-a');
  const module = nestjs.ZLinkModule.forRoot(builder.build());
  const container = await resolveModuleProviders(module, [
    nestjs.ZLINK_FRAMEWORK_RUNTIME,
    nestjs.ZLINK_ROUTE_MESH_RUNTIME,
    nestjs.ZLINK_ROUTE_CLIENT
  ]);
  const runtime = container.get(nestjs.ZLINK_FRAMEWORK_RUNTIME);
  const routeMeshRuntime = container.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME);
  const routeClient = container.get(nestjs.ZLINK_ROUTE_CLIENT);
  let remote;

  try {
    await runtime.start();
    remote = await startRouteMeshPeer('server-direct', remoteEndpoint, localEndpoint);
    await waitUntil(() => routeMeshRuntime.isReady('mesh'), 10_000);
    const reply = await routeClient
      .requestToNode('mesh', 'node-b', typedPacket('RoutePing', { value: 'ping' }))
      .timeout(1000)
      .submit();
    assert.deepEqual(reply, { value: 'pong' });

    routeClient.sendToNode('mesh', 'node-b', typedPacket('RouteNotice', { value: 'one-way' })).submit();
    const notice = await remote.next('notice');
    assert.equal(notice.value, 'one-way');
  } finally {
    await remote?.stop();
    await runtime.stop();
  }
});

test('ZLinkRoutePacketDispatcher invokes routed send and request handlers', async () => {
  const ctx = zlink.createContext();
  const localRouter = zlink.createRouterSocket(ctx);
  const remoteDealer = zlink.createDealerSocket(ctx);
  const endpoint = `inproc://route-dispatch-${process.pid}-${Date.now()}`;
  const events = [];

  try {
    localRouter.setRoutingId(zlink.RoutingId.from('node-a'));
    remoteDealer.setRoutingId(zlink.RoutingId.from('node-b'));
    localRouter.bind(endpoint);
    remoteDealer.connect(endpoint);
    const dispatcher = new framework.ZLinkRoutePacketDispatcher({
      routerChannelId: 'mesh',
      dispatchErrors: noDispatchErrorReporter(),
      handlers: [
        {
          kind: 'send',
          packetName: 'RouteNotice',
          handler: {
            async handle(payload, context) {
              events.push(`send:${context.meshName}:${context.packetName}:${payload.value}`);
            }
          }
        },
        {
          kind: 'request',
          packetName: 'RoutePing',
          handler: {
            async handle(payload, context) {
              events.push(`request:${context.meshName}:${context.packetName}:${payload.value}`);
              return { value: 'pong' };
            }
          }
        }
      ]
    });

    await submitAsyncMultipart(
      remoteDealer.send(),
      encodeDotnetEnvelope({
        kind: 3,
        channelName: 'mesh',
        messageName: 'RouteNotice',
        contentType: 'application/json',
        correlationId: null,
        deadline: null,
        topic: null,
        errorCode: null,
        errorMessage: null
      }, { value: 'one-way' })
    );
    const sent = await recvRouterMessage(localRouter);
    await dispatcher.dispatch(sent, localRouter);
    sent.close();

    const replyPromise = submitRequestMultipart(
      remoteDealer.request(),
      encodeDotnetEnvelope({
        kind: 1,
        channelName: 'mesh',
        messageName: 'RoutePing',
        contentType: 'application/json',
        correlationId: 'route-request',
        deadline: null,
        topic: null,
        errorCode: null,
        errorMessage: null
      }, { value: 'ping' })
    );
    const request = await recvRouterMessage(localRouter);
    assert.notEqual(request.replyToken, null);
    await dispatcher.dispatch(request, {
      reply() {
        return request.reply();
      }
    });
    assert.match(events[1], /^request:mesh:RoutePing:ping$/);

    const reply = await withTimeout(replyPromise, 1000, 'route dispatcher reply');
    request.close();
    const envelope = decodeDotnetEnvelope(reply);
    assert.equal(envelope.header.kind, 2);
    assert.deepEqual(envelope.body, { value: 'pong' });
    assert.equal(events[0], 'send:mesh:RouteNotice:one-way');
    assert.match(events[1], /^request:mesh:RoutePing:ping$/);
    reply.forEach((part) => part.close());
  } finally {
    remoteDealer.close();
    localRouter.close();
    ctx.close();
  }
});

test('ZLinkRoutePacketDispatcher drops route requests without a reply token without throwing', async () => {
  const reported = [];
  const dispatcher = new framework.ZLinkRoutePacketDispatcher({
    routerChannelId: 'mesh',
    dispatchErrors: {
      flow: { flowCreationEnabled() { return false; } },
      report(event) {
        reported.push(event);
      }
    },
    handlers: [
      {
        kind: 'request',
        packetName: 'RoutePing',
        handler: {
          async handle() {
            throw new Error('handler must not run without a reply path');
          }
        }
      }
    ]
  });

  const parts = encodeDotnetEnvelope({
    kind: 1,
    channelName: 'mesh',
    messageName: 'RoutePing',
    contentType: 'application/json',
    correlationId: 'missing-seq',
    deadline: null,
    topic: null,
    errorCode: null,
    errorMessage: null
  }, { value: 'ping' }).map((part) => fakeMessagePart(part));

  await dispatcher.dispatch({
    parts,
    routingId: 'node-b',
    replyToken: null,
    send() {
      throw new Error('send path must not be used for request');
    }
  }, {
    reply() {
      throw new Error('reply path must not be used without a reply token');
    }
  });

  assert.equal(reported.length, 1);
  assert.equal(reported[0].reason, 'reply_path_missing');
  assert.equal(reported[0].action, 'drop');
  assert.equal(reported[0].packetName, 'RoutePing');
  parts.forEach((part) => part.close());
});

test('ZLinkRoutePacketDispatcher forwards SPOT-addressed route frames to local SPOT delivery', async () => {
  const forwarded = [];
  let releaseSubmit;
  const submitted = new Promise((resolve) => {
    releaseSubmit = resolve;
  });
  const dispatcher = new framework.ZLinkRoutePacketDispatcher({
    routerChannelId: 'mesh',
    dispatchErrors: noDispatchErrorReporter(),
    handlers: []
  });
  const parts = [
    fakeMessagePart(Buffer.from('spot-header')),
    fakeMessagePart(Buffer.from('spot-body'))
  ];

  const consumed = dispatcher.dispatch({
    parts,
    routingId: 'node-b',
    spotId: 'room-1',
    replyToken: null,
    send() {
      return captureRawMultipart(forwarded, () => submitted);
    }
  }, {
    reply() {
      throw new Error('reply path must not be used for SPOT-addressed frame');
    }
  });

  let settled = false;
  void consumed.then(() => {
    settled = true;
  });
  await Promise.resolve();
  assert.equal(settled, false);
  releaseSubmit();

  assert.equal(forwarded.length, 2);
  assert.equal(forwarded[0].data().toString(), 'spot-header');
  assert.equal(forwarded[1].data().toString(), 'spot-body');
  assert.equal(await consumed, true);
});

test('ZLinkChannelRequestDispatcher propagates SPOT delivery submit failure', async () => {
  const failure = new Error('spot delivery failed');
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: noDispatchErrorReporter(),
    handlers: new Map()
  });

  await assert.rejects(dispatcher.dispatch({
    parts: [fakeMessagePart(Buffer.from('spot-frame'))],
    routingId: 'node-b',
    spotId: 'room-1',
    replyToken: null,
    send() {
      return captureRawMultipart([], async () => {
        throw failure;
      });
    }
  }, {
    reply() {
      throw new Error('reply path must not be used for SPOT-addressed frame');
    }
  }), failure);
});

test('route receive loop keeps receiving while an earlier SPOT forward awaits admission', async () => {
  let resolveFirstAdmission;
  let rejectFirstAdmission;
  const firstAdmission = new Promise((resolve, reject) => {
    resolveFirstAdmission = resolve;
    rejectFirstAdmission = reject;
  });
  let observeSecondSubmit;
  const secondSubmitted = new Promise((resolve) => {
    observeSecondSubmit = resolve;
  });
  const reported = [];
  const firstRecord = spotForwardRecord('first', () => firstAdmission);
  const secondRecord = spotForwardRecord('second', async () => {
    observeSecondSubmit();
  });
  const records = [
    firstRecord,
    secondRecord
  ];
  const dispatcher = new framework.ZLinkRoutePacketDispatcher({
    routerChannelId: 'mesh',
    dispatchErrors: noDispatchErrorReporter(),
    handlers: []
  });
  const router = {
    recv() {
      return records.shift();
    },
    reply() {
      throw new Error('SPOT forward must not use the reply path.');
    }
  };
  const loop = new framework.ZLinkRouteReceiveLoop(
    router,
    dispatcher,
    { wait() { return records.length > 0; }, dispose() {} },
    new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    undefined,
    (error) => reported.push(error)
  );
  const running = loop.run();

  try {
    await withTimeout(
      secondSubmitted,
      1_000,
      'second SPOT forward behind pending admission'
    );
    const failure = new Error('first SPOT admission failed');
    rejectFirstAdmission(failure);
    await waitFor(
      () => reported.length === 1,
      'pending SPOT admission failure reporting'
    );
    assert.deepEqual(reported, [failure]);
    assert.equal(firstRecord.closed, true);
    assert.equal(secondRecord.closed, false);
  } finally {
    resolveFirstAdmission();
    await loop.stop();
    await running;
  }
});

test('ZLinkRoutePacketDispatcher lets route bridge handle SPOT-addressed bridge frames first', async () => {
  const handled = [];
  const dispatcher = new framework.ZLinkRoutePacketDispatcher({
    routerChannelId: 'mesh',
    dispatchErrors: noDispatchErrorReporter(),
    handlers: [],
    spotRouteBridge: {
      handleRouterReceived(channelName, sourceNodeRid, correlation, parts) {
        handled.push({
          channelName,
          sourceNodeRid,
          correlation,
          parts: parts.map((part) => part.data().toString())
        });
        return true;
      }
    }
  });
  const parts = [
    fakeMessagePart(Buffer.from('__zlink.routed_spot.egress.request')),
    fakeMessagePart(Buffer.from('room-1')),
    fakeMessagePart(Buffer.from('payload'))
  ];

  await dispatcher.dispatch({
    parts,
    routingId: 'node-b',
    spotId: 'room-1',
    replyToken: {},
    send() {
      throw new Error('direct SPOT delivery must not run for bridge frames');
    }
  }, {
    reply() {
      throw new Error('reply path must not be used directly');
    }
  });

  assert.deepEqual(handled, [{
    channelName: 'mesh',
    sourceNodeRid: 'node-b',
    correlation: 0n,
    parts: ['__zlink.routed_spot.egress.request', 'room-1', 'payload']
  }]);
});

test('ZLinkModule route channel dispatches inbound routed handlers after bootstrap', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const remoteEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const events = [];
  const filterContexts = [];
  class NodeDirectFilter {
    async invoke(context, next) {
      filterContexts.push(context);
      return next();
    }
  }
  class RouteNoticeHandler {
    async handle(payload, context) {
      events.push(`send:${context.meshName}:${context.packetName}:${payload.value}`);
    }
  }
  class RoutePingHandler {
    async handle(payload, context) {
      events.push(`request:${context.meshName}:${context.packetName}:${payload.value}`);
      return { value: 'pong' };
    }
  }
  class HandlerModule {}
  const builder = nestjs.zlinkFramework()
    .options({ filters: [NodeDirectFilter] });
  const mesh = builder
    .addRouteMesh('mesh')
      .listen(endpoint)
      .routingId('node-a');
  mesh.addSendHandler('RouteNotice', RouteNoticeHandler);
  mesh.addRequestHandler('RoutePing', RoutePingHandler);
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [NodeDirectFilter, RouteNoticeHandler, RoutePingHandler]
  })(HandlerModule);
  const app = await NestFactory.createApplicationContext(HandlerModule, { logger: false, abortOnError: false });
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  let remote;

  try {
    await runtime.start();
    remote = await startRouteMeshPeer('client-direct', remoteEndpoint, endpoint);
    remote.send({ type: 'run', mode: 'direct' });
    const reply = (await remote.next('result')).result;
    assert.deepEqual(reply, { value: 'pong' });
    assert.equal(events.includes('send:mesh:RouteNotice:one-way'), true);
    assert.equal(events.includes('request:mesh:RoutePing:ping'), true);
    const dispatchKinds = filterContexts.map((context) => context.dispatchKind);
    assert.equal(dispatchKinds.includes(framework.ZLinkHandlerDispatchKind.NodeDirectRequest), true);
    assert.equal(dispatchKinds.includes(framework.ZLinkHandlerDispatchKind.NodeDirectSend), true);
    assert.equal(dispatchKinds.every((kind) =>
      kind === framework.ZLinkHandlerDispatchKind.NodeDirectRequest ||
      kind === framework.ZLinkHandlerDispatchKind.NodeDirectSend), true);
    assert.equal(filterContexts.every((context) => context.channelName === undefined), true);
  } finally {
    await remote?.stop();
    await runtime.stop();
    await app.close();
  }
});

test('ZLinkModule routeMesh channel option dispatches inbound routed handlers after bootstrap', async () => {
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const remoteEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const events = [];
  const filterContexts = [];
  class ChannelOnlyFilter {
    async invoke(context, next) {
      filterContexts.push(context);
      return next();
    }
  }
  class RoutePingHandler {
    async handle(payload, context) {
      events.push(`request:${context.meshName}:${context.packetName}:${payload.value}`);
      return { value: 'pong' };
    }
  }
  class HandlerModule {}
  const builder = nestjs.zlinkFramework()
    .options({ filters: [ChannelOnlyFilter] });
  const mesh = builder
    .addRouteMesh('mesh')
      .listen(endpoint)
      .routingId('node-a');
  mesh.channel('mesh')
    .server()
    .addRequestHandler('RoutePing', RoutePingHandler);
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [ChannelOnlyFilter, RoutePingHandler]
  })(HandlerModule);
  const app = await NestFactory.createApplicationContext(HandlerModule, { logger: false, abortOnError: false });
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  let remote;

  try {
    await runtime.start();
    remote = await startRouteMeshPeer('client-channel', remoteEndpoint, endpoint);
    remote.send({ type: 'run', mode: 'channel' });
    const reply = (await remote.next('result')).result;
    assert.deepEqual(reply, { value: 'pong' });
    assert.equal(events[0], 'request:mesh:RoutePing:ping');
    assert.equal(filterContexts.length, 1);
    assert.equal(
      filterContexts[0].dispatchKind,
      framework.ZLinkHandlerDispatchKind.ChannelRequest
    );
    assert.equal(filterContexts[0].meshName, 'mesh');
    assert.equal(filterContexts[0].channelName, 'mesh');
  } finally {
    await remote?.stop();
    await runtime.stop();
    await app.close();
  }
});

test('PUB-001 partial ZLinkFanoutClient publishes through public pub/sub binding sockets', async () => {
  const ctx = zlink.createContext();
  const pub = zlink.createPubSocket(ctx);
  const sub = zlink.createSubSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const endpoint = `inproc://framework-channel-publish-${process.pid}-${Date.now()}`;
  const topic = 'Event';

  try {
    pub.bind(endpoint);
    sub.setSubscription(topic);
    sub.recvTimeout = 20;
    sub.connect(endpoint);

    const registration = framework.createFrameworkRegistration({
      channels: { events: { publisher: { bind: endpoint } } }
    });
    const fanout = new framework.DefaultZLinkFanoutClient(
      registration,
      new framework.ZLinkDealerChannelClientTransport(dealer, pub)
    );

    const received = new zlink.TopicMessage();
    try {
      await publishUntilSubscribed(fanout, sub, received, topic, 'Event', 'published');
      assert.equal(received.topic, topic);
      const envelope = decodeDotnetEnvelope(received.parts);
      assert.equal(envelope.header.kind, 4);
      assert.equal(envelope.header.channelName, 'events');
      assert.equal(envelope.header.messageName, 'Event');
      assert.equal(envelope.header.topic, topic);
      assert.equal(envelope.body, 'published');
    } finally {
      received.close();
    }
  } finally {
    dealer.close();
    sub.close();
    pub.close();
    ctx.close();
  }
});

test('classic fanout filters receive a minimal context and isolate each handler invocation', async () => {
  const filterContexts = [];
  const handled = [];
  const dispatcher = new framework.ZLinkChannelPublishDispatcher({
    // Classic fanout is not a MeshNode member. Even a stale internal value
    // must not leak through the public filter context.
    meshName: 'internal-fanout-placeholder',
    channelName: 'events',
    dispatchErrors: noDispatchErrorReporter(),
    handlers: new Map([
      ['Event', {
        async handle(payload) {
          handled.push(payload.value);
        }
      }]
    ]),
    filters: [{
      async invoke(context, next) {
        filterContexts.push(context);
        await next();
      }
    }]
  });
  const parts = encodeDotnetEnvelope({
    kind: 4,
    channelName: 'events',
    messageName: 'Event',
    contentType: 'application/json',
    correlationId: null,
    deadline: null,
    topic: 'room',
    source: 'publisher-a',
    errorCode: null,
    errorMessage: null
  }, { value: 'first' }).map(fakeMessagePart);
  const nextParts = encodeDotnetEnvelope({
    kind: 4,
    channelName: 'events',
    messageName: 'Event',
    contentType: 'application/json',
    correlationId: null,
    deadline: null,
    topic: 'room',
    source: 'publisher-a',
    errorCode: null,
    errorMessage: null
  }, { value: 'second' }).map(fakeMessagePart);

  try {
    await dispatcher.dispatch({ topic: 'room', parts });
    await dispatcher.dispatch({ topic: 'room', parts: nextParts });
    assert.deepEqual(handled, ['first', 'second']);
    assert.equal(filterContexts.length, 2);
    assert.notEqual(filterContexts[0], filterContexts[1]);
    assert.equal(
      filterContexts[0].dispatchKind,
      framework.ZLinkHandlerDispatchKind.ClassicFanout
    );
    assert.equal(filterContexts[0].meshName, undefined);
    assert.equal(filterContexts[0].channelName, 'events');
    assert.equal('topic' in filterContexts[0], false);
    assert.equal('source' in filterContexts[0], false);
  } finally {
    parts.forEach((part) => part.close?.());
    nextParts.forEach((part) => part.close?.());
  }
});

test('ZLinkChannelRequestDispatcher invokes request handler and replies through router', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const endpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const filterEvents = [];
  let routerMonitor;
  let dealerMonitor;

  try {
    routerMonitor = router.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    dealerMonitor = dealer.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    router.bind(endpoint);
    dealer.connect(endpoint);
    await waitForMonitorConnectionReady(routerMonitor, 'channel dispatcher router connection', router);
    await waitForMonitorConnectionReady(dealerMonitor, 'channel dispatcher dealer connection', router);
    routerMonitor.close();
    routerMonitor = null;
    dealerMonitor.close();
    dealerMonitor = null;

    const registration = framework.createFrameworkRegistration({
      channels: { api: { client: { manualConnections: [endpoint] } } }
    });
    const client = new framework.DefaultZLinkChannelClient(
      registration,
      new framework.ZLinkDealerChannelClientTransport(dealer)
    );
    const dispatcher = new framework.ZLinkChannelRequestDispatcher({
      channelName: 'api',
      dispatchErrors: noDispatchErrorReporter(),
      handlers: new Map([
        ['Ping', {
          async handle(payload) {
            assert.equal(payload, 'ping');
            return 'pong';
          }
        }]
      ]),
      filters: [{
        async invoke(_invocation, next) {
          filterEvents.push('before');
          const result = await next();
          filterEvents.push('after');
          return result;
        }
      }]
    });

    const replyPromise = client.requestToChannel('api', typedPacket('Ping', 'ping')).timeout(1000).submit();
    const received = await recvRouterMessage(router);
    await dispatcher.dispatch(received, router);

    const reply = await withTimeout(replyPromise, 1000, 'framework dispatcher reply');
    assert.equal(reply, 'pong');
    assert.deepEqual(filterEvents, ['before', 'after']);
    received.close();
  } finally {
    dealerMonitor?.close();
    routerMonitor?.close();
    dealer.close();
    router.close();
    ctx.close();
  }
});

test('ZLinkChannelRequestDispatcher preserves source node identity for RouteMesh channels', async () => {
  let invocation;
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    meshName: 'zone-mesh',
    channelName: 'report',
    routeMesh: true,
    dispatchErrors: noDispatchErrorReporter(),
    handlers: new Map(),
    sendHandlers: new Map([
      ['NodeStatus', {
        async handle(payload, context) {
          invocation = { payload, context };
        }
      }]
    ])
  });
  const parts = encodeDotnetEnvelope({
    kind: 3,
    channelName: 'report',
    messageName: 'NodeStatus',
    contentType: 'application/json',
    correlationId: null,
    deadline: null,
    topic: null,
    errorCode: null,
    errorMessage: null
  }, { nodeId: 'zone-node-1' }).map(fakeMessagePart);

  await dispatcher.dispatchMesh({
    parts,
    sourceNodeRid: 'node-rid-1'
  });

  assert.deepEqual(invocation.payload, { nodeId: 'zone-node-1' });
  assert.equal(invocation.context.meshName, 'zone-mesh');
  assert.equal(invocation.context.channelName, 'report');
  assert.equal(invocation.context.sourceNodeRid, 'node-rid-1');
  parts.forEach((part) => part.close());
});

test('ZLinkChannelRequestDispatcher replies error and reports provider record for missing request handler', async () => {
  telemetry.reset();
  const events = telemetry.records
  const replies = [];
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: dispatchErrorReporter(
      undefined,
      { reportRuntimeTaskException() {} }
    ),
    handlers: new Map(),
    sendHandlers: new Map()
  });
  const parts = encodeDotnetEnvelope({
    kind: 1,
    channelName: 'api',
    messageName: 'MissingReq',
    contentType: 'application/json',
    correlationId: 'corr-1',
    deadline: null,
    topic: null,
    errorCode: null,
    errorMessage: null
  }, { value: 'request' }).map(fakeMessagePart);
  const router = {
    reply() {
      return captureMultipart(replies);
    }
  };

  await dispatcher.dispatch({
    parts,
    routingId: 'client-1',
    replyToken: {}
  }, router);
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(replies.length, 2);
  const replyEnvelope = decodeDotnetEnvelope(replies);
  assert.equal(replyEnvelope.header.kind, 5);
  assert.equal(replyEnvelope.header.correlationId, 'corr-1');
  assert.equal(events.length, 1);
  assert.equal(events[0].surface, 'channel');
  assert.equal(events[0].messageKind, 'request');
  assert.equal(events[0].outcome, 'failed');
  assert.equal(events[0].reason, 'no_handler');
  assert.equal(events[0].action, 'reply_error');
  assert.equal(events[0].packetName, 'MissingReq');
  assert.equal(events[0].channelName, 'api');
  assert.equal(events[0].correlationId, 'corr-1');
});

test('ZLinkChannelRequestDispatcher rejects filter short circuit without serializing its return value', async () => {
  let handlerInvocations = 0;
  const filterContexts = [];
  const replies = [];
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    meshName: 'mesh-a',
    channelName: 'api',
    dispatchErrors: noDispatchErrorReporter(),
    handlers: new Map([
      ['Ping', {
        handle() {
          handlerInvocations += 1;
          return 'handler-reply';
        }
      }]
    ]),
    filters: [{
      async invoke(context) {
        filterContexts.push(context);
        return 'filter-reply';
      }
    }]
  });
  const parts = encodeDotnetEnvelope({
    kind: 1,
    channelName: 'api',
    messageName: 'Ping',
    contentType: 'application/json',
    correlationId: 'corr-filter-stop',
    deadline: null,
    topic: null,
    errorCode: null,
    errorMessage: null
  }, { value: 'request' }).map(fakeMessagePart);

  await dispatcher.dispatch({
    parts,
    routingId: 'client-1',
    replyToken: {}
  }, {
    reply() {
      return captureMultipart(replies);
    }
  });

  assert.equal(handlerInvocations, 0);
  assert.equal(filterContexts.length, 1);
  assert.equal(
    filterContexts[0].dispatchKind,
    framework.ZLinkHandlerDispatchKind.ChannelRequest
  );
  assert.equal(filterContexts[0].meshName, 'mesh-a');
  assert.equal('sourceNodeRid' in filterContexts[0], false);
  const replyEnvelope = decodeDotnetEnvelope(replies);
  assert.equal(replyEnvelope.header.kind, 5);
  assert.equal(replyEnvelope.header.errorCode, 'rejected');
  assert.notEqual(replyEnvelope.body, 'filter-reply');
});

test('ZLinkChannelRequestDispatcher drops requests without a reply token without invoking handlers', async () => {
  telemetry.reset();
  const events = telemetry.records;
  let handlerInvocations = 0;
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: dispatchErrorReporter(
      undefined,
      { reportRuntimeTaskException() {} }
    ),
    handlers: new Map([
      ['Ping', {
        handle() {
          handlerInvocations += 1;
          return 'pong';
        }
      }]
    ])
  });
  const parts = encodeDotnetEnvelope({
    kind: 1,
    channelName: 'api',
    messageName: 'Ping',
    contentType: 'application/json',
    correlationId: 'corr-no-seq',
    deadline: null,
    topic: null,
    errorCode: null,
    errorMessage: null
  }, 'ping').map(fakeMessagePart);

  await dispatcher.dispatch({
    parts,
    routingId: 'client-1',
    replyToken: null
  }, {
    reply() {
      assert.fail('request without a reply token must not create a reply operation');
    }
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(handlerInvocations, 0);
  assert.equal(events.length, 1);
  assert.equal(events[0].surface, 'channel');
  assert.equal(events[0].messageKind, 'request');
  assert.equal(events[0].reason, 'reply_path_missing');
  assert.equal(events[0].action, 'drop');
  assert.equal(events[0].packetName, 'Ping');
  assert.equal(events[0].correlationId, 'corr-no-seq');
});

test('DERR-006 ZLinkChannelRequestDispatcher replies error and reports provider record for payload decode failure', async () => {
  telemetry.reset();
  const events = telemetry.records;
  const runtimeFailures = [];
  let handlerInvocations = 0;
  const replies = [];
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: dispatchErrorReporter(
      undefined,
      {
        reportRuntimeTaskException(taskName, error) {
          runtimeFailures.push({ taskName, error });
        }
      }
    ),
    handlers: new Map([
      ['BrokenReq', {
        handle() {
          handlerInvocations += 1;
          return { value: 'unexpected' };
        }
      }],
      ['KnownReq', {
        handle(payload) {
          return { value: `known:${payload.value}` };
        }
      }]
    ]),
    sendHandlers: new Map()
  });
  const parts = [
    fakeMessagePart(Buffer.from(JSON.stringify({
      formatMarker: 0xf2,
      flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
      flowOrigin: 1,
      kind: 1,
      channelName: 'api',
      messageName: 'BrokenReq',
      contentType: 'application/json',
      correlationId: 'corr-decode',
      deadline: null,
      topic: null,
      errorCode: null,
      errorMessage: null
    }))),
    fakeMessagePart(Buffer.from('{'))
  ];
  const router = {
    reply() {
      return captureMultipart(replies);
    }
  };

  await dispatcher.dispatch({
    parts,
    routingId: 'client-1',
    replyToken: {}
  }, router);
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(handlerInvocations, 0);
  assert.equal(replies.length, 2);
  const replyEnvelope = decodeDotnetEnvelope(replies);
  assert.equal(replyEnvelope.header.kind, 5);
  assert.equal(replyEnvelope.header.correlationId, 'corr-decode');
  assert.match(replyEnvelope.header.errorMessage, /PayloadDecodeFailed/);
  assert.equal(events.length, 1);
  assert.equal(events[0].surface, 'channel');
  assert.equal(events[0].messageKind, 'request');
  assert.equal(events[0].outcome, 'failed');
  assert.equal(events[0].reason, 'decode_error');
  assert.equal(events[0].action, 'reply_error');
  assert.equal(events[0].packetName, 'BrokenReq');
  assert.equal(events[0].channelName, 'api');
  assert.equal(events[0].correlationId, 'corr-decode');
  assert.equal(events[0].errorType, 'ZLinkFrameworkException');
  assert.match(events[0].errorMessage, /PayloadDecodeFailed/);
  assert.equal(runtimeFailures.length, 0);

  replies.length = 0;
  await dispatcher.dispatch({
    parts: encodeDotnetEnvelope({
      kind: 1,
      channelName: 'api',
      messageName: 'KnownReq',
      contentType: 'application/json',
      correlationId: 'corr-after-decode',
      deadline: null,
      topic: null,
      errorCode: null,
      errorMessage: null
    }, { value: 'after-decode-error' }).map(fakeMessagePart),
    routingId: 'client-1',
    replyToken: {}
  }, router);
  const afterEnvelope = decodeDotnetEnvelope(replies);
  assert.equal(afterEnvelope.header.kind, 2);
  assert.deepEqual(afterEnvelope.body, { value: 'known:after-decode-error' });
});

test('ZLinkChannelRequestDispatcher submits error replies directly to the completion lane', async () => {
  const replies = [];
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: noDispatchErrorReporter(),
    handlers: new Map([
      ['ThrowReq', {
        handle() {
          throw new Error('deterministic handler failure');
        }
      }]
    ]),
    sendHandlers: new Map()
  });
  const router = {
    attempts: 0,
    routingId: undefined,
    replyToken: undefined,
    reply(routingId, replyToken) {
      this.attempts += 1;
      this.routingId = routingId;
      this.replyToken = replyToken;
      return captureMultipart(replies);
    }
  };

  const replyToken = {};
  await dispatcher.dispatch({
    parts: encodeDotnetEnvelope({
      kind: 1,
      channelName: 'api',
      messageName: 'ThrowReq',
      contentType: 'application/json',
      correlationId: 'corr-backpressured-error',
      deadline: null,
      topic: null,
      errorCode: null,
      errorMessage: null
    }, { value: 'boom' }).map(fakeMessagePart),
    routingId: 'client-1',
    replyToken
  }, router);

  assert.equal(router.attempts, 1);
  assert.equal(router.routingId, 'client-1');
  assert.equal(router.replyToken, replyToken);
  assert.equal(replies.length, 2);
  const replyEnvelope = decodeDotnetEnvelope(replies);
  assert.equal(replyEnvelope.header.kind, 5);
  assert.equal(replyEnvelope.header.correlationId, 'corr-backpressured-error');
  assert.match(replyEnvelope.header.errorMessage, /deterministic handler failure/);
});

test('self-RID RouteMesh waits on the shared application job queue without local backpressure', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      applicationJobQueue: { maxQueuedApplicationJobs: 1n }
    })
  });
  const scheduled = [];
  let handled = 0;
  let releaseFirstHandler;
  const firstHandlerGate = new Promise((resolve) => {
    releaseFirstHandler = resolve;
  });
  host.admission.register('mesh');
  host.executionState = {
    abortController: new AbortController(),
    taskRunner: {
      runDetached(_name, callback) {
        scheduled.push(callback);
      }
    }
  };
  host.channelRuntime = {
    canDispatchLocalMeshRoute: () => true,
    async dispatchLocalMeshRoute() {
      handled += 1;
      if (handled === 1) await firstHandlerGate;
    }
  };
  host.spotNodeRuntime = {
    meshNode: () => ({ status: () => ({ routingId: zlink.RoutingId.from('self-node') }) }),
    meshCompletionTable: () => undefined
  };

  const first = host.routeTransport.submit('mesh', 'self-node', 'Notice', { sequence: 1 });
  assert.deepEqual(await first, { status: ZLinkSubmitStatus.Submitted });
  assert.equal(scheduled.length, 1);

  const second = host.routeTransport.submit('mesh', 'self-node', 'Notice', { sequence: 2 });
  assert.deepEqual(await second, { status: ZLinkSubmitStatus.Submitted });
  assert.equal(scheduled.length, 2);

  const cancelledController = new AbortController();
  cancelledController.abort();
  const cancelled = host.routeTransport.submit(
    'mesh',
    'self-node',
    'Notice',
    { sequence: 3 },
    cancelledController.signal
  );
  await assert.rejects(cancelled, (error) => error?.name === 'AbortError');
  assert.equal(scheduled.length, 2);

  const firstDispatch = scheduled.shift()();
  await waitUntil(() => handled === 1);
  const secondDispatch = scheduled.shift()();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(handled, 1);
  assert.equal(host.applicationJobQueue.snapshot().capacityWaiters, 1n);
  releaseFirstHandler();
  await Promise.all([firstDispatch, secondDispatch]);
  assert.equal(scheduled.length, 0);

  const recovered = host.routeTransport.submit('mesh', 'self-node', 'Notice', { sequence: 4 });
  assert.deepEqual(await recovered, { status: ZLinkSubmitStatus.Submitted });
  assert.equal(scheduled.length, 1);
  await scheduled.shift()();
  assert.equal(handled, 3);
  assert.equal(host.applicationJobQueue.snapshot().permitsInUse, 0n);
});

test('DERR-009 ZLinkChannelRequestDispatcher writes structured dispatch errors to the logger provider', async () => {
  telemetry.reset();
  const events = telemetry.records;
  const dispatcher = new framework.ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: dispatchErrorReporter(
      undefined,
      { reportRuntimeTaskException() {} }
    ),
    handlers: new Map([
      ['DecodeReq', {
        handle() {
          return { value: 'unexpected' };
        }
      }],
      ['ThrowReq', {
        handle() {
          throw new Error('DERR-007 handler exception');
        }
      }]
    ]),
    sendHandlers: new Map()
  });
  const router = {
    reply() {
      return captureMultipart([]);
    }
  };

  await dispatcher.dispatch({
      parts: encodeDotnetEnvelope({
        kind: 1,
        channelName: 'api',
        messageName: 'MissingReq',
        contentType: 'application/json',
        correlationId: 'corr-missing',
        deadline: null,
        topic: null,
        errorCode: null,
        errorMessage: null
      }, { value: 'missing' }).map(fakeMessagePart),
      routingId: 'client-1',
      replyToken: {}
    }, router);
    await dispatcher.dispatch({
      parts: [
        fakeMessagePart(Buffer.from(JSON.stringify({
          formatMarker: 0xf2,
          flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
          flowOrigin: 1,
          kind: 1,
          channelName: 'api',
          messageName: 'DecodeReq',
          contentType: 'application/json',
          correlationId: 'corr-decode',
          deadline: null,
          topic: null,
          errorCode: null,
          errorMessage: null
        }))),
        fakeMessagePart(Buffer.from('{'))
      ],
      routingId: 'client-1',
      replyToken: {}
    }, router);
    await dispatcher.dispatch({
      parts: encodeDotnetEnvelope({
        kind: 1,
        channelName: 'api',
        messageName: 'ThrowReq',
        contentType: 'application/json',
        correlationId: 'corr-throw',
        deadline: null,
        topic: null,
        errorCode: null,
        errorMessage: null
      }, { value: 'boom' }).map(fakeMessagePart),
      routingId: 'client-1',
      replyToken: {}
    }, router);
  assert.equal(events.length, 3);
  assert.deepEqual(events.map((event) => event.eventId), [
    'zlink.dispatch_error',
    'zlink.dispatch_error',
    'zlink.dispatch_error'
  ]);
  assert.deepEqual(events.map((event) => event.reason), [
    'no_handler',
    'decode_error',
    'handler_exception'
  ]);
  assert.deepEqual(events.map((event) => event.packetName), [
    'MissingReq',
    'DecodeReq',
    'ThrowReq'
  ]);
  assert.equal(events[1].correlationId, 'corr-decode');
});

test('channel dispatch failures emit one logger-provider record per failure', () => {
  telemetry.reset();
  const reporter = noDispatchErrorReporter();
    reporter.report({
      surface: 'channel',
      messageKind: 'send',
      reason: 'no_handler',
      action: 'drop',
      packetName: 'MissingSend'
    });
    reporter.report({
      surface: 'channel',
      messageKind: 'send',
      reason: 'decode_error',
      action: 'drop',
      packetName: 'BrokenPublish'
    });
    reporter.report({
      surface: 'channel',
      messageKind: 'send',
      reason: 'handler_exception',
      action: 'drop',
      packetName: 'ThrowingPublish',
      error: new Error('application failure')
    });

  assert.equal(telemetry.records.length, 3);
  assert.deepEqual(
    telemetry.records.map((event) => event.packetName),
    ['MissingSend', 'BrokenPublish', 'ThrowingPublish']
  );
  assert.equal(telemetry.records[2].reason, 'handler_exception');
});

async function recvRouterMessage(router) {
  const deadline = Date.now() + 1000;
  while (Date.now() < deadline) {
    const received = new zlink.Received();
    if (router.recv(received, zlink.RecvFlags.DontWait)) {
      return received;
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail('router did not receive request');
}

async function recvRoutedEnvelopeMessage(router) {
  const deadline = Date.now() + 1000;
  while (Date.now() < deadline) {
    const received = new zlink.Received();
    if (router.recv(received, zlink.RecvFlags.DontWait)) {
      if (received.parts.length >= 2 && received.parts[0].data().length > 0) {
        return received;
      }
      received.close();
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail('router did not receive routed envelope');
}

async function startRouteMeshPeer(mode, bind, peer) {
  const child = fork(
    path.join(__dirname, 'helpers', 'route-mesh-peer.js'),
    [JSON.stringify({ mode, bind, peer })],
    { stdio: ['ignore', 'pipe', 'pipe', 'ipc'] }
  );
  let stderr = '';
  const queued = [];
  const waiters = [];
  child.stderr.on('data', chunk => {
    stderr += chunk.toString();
  });
  child.on('message', message => {
    const waiterIndex = waiters.findIndex(waiter => waiter.type === message?.type);
    if (waiterIndex >= 0) {
      const [waiter] = waiters.splice(waiterIndex, 1);
      waiter.resolve(message);
      return;
    }
    if (message?.type === 'error') {
      const error = new Error(message.message);
      for (const waiter of waiters.splice(0)) waiter.reject(error);
      return;
    }
    queued.push(message);
  });
  child.on('exit', (code, signal) => {
    if (code === 0 && waiters.length === 0) return;
    const error = new Error(
      `RouteMesh peer exited code=${code} signal=${signal}.${stderr.length === 0 ? '' : `\n${stderr}`}`
    );
    for (const waiter of waiters.splice(0)) waiter.reject(error);
  });

  const next = (type, timeoutMs = 10000) => {
    const queuedIndex = queued.findIndex(message => message?.type === type);
    if (queuedIndex >= 0) {
      return Promise.resolve(queued.splice(queuedIndex, 1)[0]);
    }
    return new Promise((resolve, reject) => {
      const waiter = { type, resolve, reject };
      waiters.push(waiter);
      const timer = setTimeout(() => {
        const index = waiters.indexOf(waiter);
        if (index >= 0) waiters.splice(index, 1);
        reject(new Error(`Timed out waiting for RouteMesh peer '${type}'.${stderr.length === 0 ? '' : `\n${stderr}`}`));
      }, timeoutMs);
      waiter.resolve = message => {
        clearTimeout(timer);
        resolve(message);
      };
      waiter.reject = error => {
        clearTimeout(timer);
        reject(error);
      };
    });
  };

  await next('ready');
  return {
    send(message) {
      child.send(message);
    },
    next,
    async stop() {
      if (child.exitCode !== null || child.signalCode !== null) return;
      child.send({ type: 'stop' });
      await once(child, 'exit');
    }
  };
}

async function submitWhenReachable(submit) {
  const deadline = Date.now() + 1000;
  let lastError;
  while (Date.now() < deadline) {
    try {
      return await submit();
    } catch (error) {
      if (!isHostUnreachable(error)) {
        throw error;
      }
      lastError = error;
      await new Promise((resolve) => setImmediate(resolve));
    }
  }
  throw lastError;
}

function createScaleoutProvider(locationStore, bindEndpoint, providerId, routingId) {
  const evidence = [];
  const registration = framework.createFrameworkRegistration({
    locations: { storeInstance: locationStore, options: scaleoutLocationOptions() },
    channels: {
      'scaleout-api': {
        server: {
          bind: bindEndpoint,
          routingId
        },
        requestHandlers: [{
          packetName: 'ScaleoutProbe',
          handler: {
            handle(payload) {
              evidence.push(payload.requestId);
              return {
                requestId: payload.requestId,
                providerId
              };
            }
          }
        }]
      }
    }
  });
  return {
    evidence,
    runtime: new framework.ZLinkFrameworkRuntimeHost({ registration })
  };
}

function createRoundRobinServer(bindEndpoint, serverId) {
  const evidence = [];
  const registration = framework.createFrameworkRegistration({
    channels: {
      'round-robin': {
        server: { bind: bindEndpoint, routingId: serverId },
        requestHandlers: [{
          packetName: 'RoundRobinProbe',
          handler: {
            handle(payload, context) {
              assert.equal(context.channelName, 'round-robin');
              assert.equal(context.packetName, 'RoundRobinProbe');
              evidence.push(payload.requestId);
              return { requestId: payload.requestId, serverId };
            }
          }
        }]
      }
    }
  });
  return {
    serverId,
    evidence,
    runtime: new framework.ZLinkFrameworkRuntimeHost({ registration })
  };
}

function countBy(values) {
  const counts = {};
  for (const value of values) {
    counts[value] = (counts[value] ?? 0) + 1;
  }
  return counts;
}

function assertRoundRobinCounts(counts) {
  assert.deepEqual(counts, {
    'server-a': 30,
    'server-b': 30,
    'server-c': 30
  });
}

async function createScaleoutClientApp(locationStore) {
  class ScaleoutClientModule {}
  const builder = nestjs.zlinkFramework().addLocationStore(locationStore);
  const options = scaleoutLocationOptions();
  builder.configureLocations()
    .pollingIntervalMs(options.pollingIntervalMs)
    .ownerLeaseRenewIntervalMs(options.ownerLeaseRenewIntervalMs)
    .ownerLeaseTtlMs(options.ownerLeaseTtlMs);
  builder.addClientServerChannel('scaleout-api').client();
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())]
  })(ScaleoutClientModule);
  return NestFactory.createApplicationContext(ScaleoutClientModule, { logger: false });
}

function scaleoutLocationOptions() {
  return {
    pollingIntervalMs: 20,
    ownerLeaseRenewIntervalMs: 5000,
    ownerLeaseTtlMs: 30000
  };
}

async function waitForAutoConnectPollCycle() {
  await new Promise((resolve) => setTimeout(resolve, scaleoutLocationOptions().pollingIntervalMs * 3));
}

function requestScaleoutProbe(client, requestId) {
  return withAbortTimeout(
    (signal) => submitWhenReachable(() =>
      client
        .requestToChannel('scaleout-api', typedPacket('ScaleoutProbe', { requestId }))
        .timeout(1000)
        .submit(signal)
    ),
    1500,
    `scaleout probe ${requestId}`
  );
}

async function runScaleoutTrafficBatch(clients, label, count) {
  const traffic = {
    completedRequestIds: [],
    observedProviders: new Set()
  };
  for (let index = 0; index < count; index += 1) {
    const client = clients[index % clients.length];
    const requestId = `${label}-${index}`;
    const reply = await requestScaleoutProbe(client, requestId);
    traffic.completedRequestIds.push(requestId);
    traffic.observedProviders.add(reply.providerId);
  }
  return traffic;
}

async function waitForScaleoutTrafficProviders(clients, label, providers) {
  const deadline = Date.now() + 5000;
  const traffic = {
    completedRequestIds: [],
    observedProviders: new Set()
  };
  let index = 0;
  while (Date.now() < deadline) {
    const client = clients[index % clients.length];
    const requestId = `${label}-${index}`;
    index += 1;
    const reply = await requestScaleoutProbe(client, requestId);
    traffic.completedRequestIds.push(requestId);
    traffic.observedProviders.add(reply.providerId);
    if (providers.every((provider) => traffic.observedProviders.has(provider))) {
      return traffic;
    }
  }
  assert.fail(
    `HAR-007 classification-required labels=core-capi,bindings,framework,sample,harness: transition traffic did not observe providers ${providers.join(',')}`
  );
}

function scaleoutPeer(owner, nodeRid, endpoint) {
  return {
    channelName: 'scaleout-api',
    serverRid: zlink.RoutingId.from(nodeRid),
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    endpoint,
    weight: 100,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'scaleout',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: new Date(0)
  };
}

function assertRequestIdsHandledOnce(requestIds, ...providers) {
  for (const requestId of requestIds) {
    const count = providers.reduce((sum, provider) =>
      sum + provider.evidence.filter((observed) => observed === requestId).length, 0);
    assert.equal(count, 1, `${requestId} should be handled exactly once`);
  }
}

async function waitForReadyEndpoints(store, endpoints) {
  const expected = new Set(endpoints);
  await waitForScaleoutPeers(store, (entries) =>
    [...expected].every((endpoint) => entries.some((entry) => entry.endpoint === endpoint))
  );
}

async function waitUntilEndpointIsNotReady(store, endpoint) {
  await waitForScaleoutPeers(store, (entries) => entries.every((entry) => entry.endpoint !== endpoint));
}

async function waitForSingleReadyEndpoint(store, routingId, endpoint) {
  await waitForScaleoutPeers(store, (entries) =>
    entries.length === 1 && String(entries[0].serverRid) === routingId && entries[0].endpoint === endpoint,
    routingId
  );
}

async function waitForScaleoutPeers(store, predicate, routingId) {
  const deadline = Date.now() + 5000;
  let lastEntries = [];
  while (Date.now() < deadline) {
    const page = await store.listClientServers('scaleout-api', { pageSize: 1000 });
    lastEntries = page.items.filter((entry) =>
      routingId === undefined || String(entry.serverRid) === routingId);
    if (predicate(lastEntries)) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  assert.fail(
    `HAR-007 classification-required labels=core-capi,bindings,framework,sample,harness: location peers did not converge: ${JSON.stringify(lastEntries)}`
  );
}

function isHostUnreachable(error) {
  if (error instanceof framework.ZLinkFrameworkException) {
    return error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      && !('isRetriable' in error);
  }
  return error instanceof Error &&
    (((error.code === 2 || error.code === 12) && /Host unreachable/.test(error.message)) ||
      (error.code === 5 && /Connection refused/.test(error.message)));
}

async function waitFor(predicate, label) {
  const deadline = Date.now() + 1000;
  while (Date.now() < deadline) {
    if (predicate()) {
      return;
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail(`${label} did not complete`);
}

async function waitForMonitorConnectionReady(monitor, label, activitySocket) {
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  poller.add(activitySocket, [zlink.PollEventFlag.PollIn], 0);
  const deadline = Date.now() + 1000;
  try {
    while (Date.now() < deadline) {
      try {
        const event = monitor.recv(zlink.RecvFlags.DontWait);
        if (event?.event === zlink.MonitorEventType.ConnectionReady) {
          return;
        }
      } catch (error) {
        if (!(error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData)) {
          throw error;
        }
      }
      poller.wait(events, Math.min(10, deadline - Date.now()));
    }
  } finally {
    events.close();
    poller.close();
  }
  assert.fail(`${label} did not complete`);
}

async function publishUntilCommonFanoutSequence(fanout, _topic, first, second, third) {
  const deadline = Date.now() + 3000;
  let sequence = 0;
  while (Date.now() < deadline) {
    const common = commonFanoutSequence(first, second, third);
    if (common !== undefined) {
      return common;
    }
    await fanout
      .publish('events', typedPacket('ProfileChanged', { sequence, profileId: `p${sequence}` }))
      .submit();
    sequence += 1;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  const common = commonFanoutSequence(first, second, third);
  if (common !== undefined) {
    return common;
  }
  assert.fail(`fanout sequence did not reach all subscribers: first=${JSON.stringify(first)}, second=${JSON.stringify(second)}, third=${JSON.stringify(third)}`);
}

async function publishUntilHandled(fanout, channelName, _topic, packetName, payload, predicate) {
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
    await fanout
      .publish(channelName, typedPacket(packetName, payload))
      .submit();
    if (predicate()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.fail(`publish handler evidence did not appear for ${channelName}:${packetName}`);
}

function commonFanoutSequence(first, second, third) {
  const secondSequences = new Set(second.map((call) => call.payload.sequence));
  const thirdSequences = new Set(third.map((call) => call.payload.sequence));
  return first
    .map((call) => call.payload.sequence)
    .find((sequence) => secondSequences.has(sequence) && thirdSequences.has(sequence));
}

//  surface 기본값은 channel이다. Classic fanout subscriber의 handler 부재는
//  surface=classic_fanout, message_kind=send로 기록한다 — Message flow tracing 3.1과
//  그 뒤의 Classic fanout 규정. message_kind 허용 값에 publish는 없다.
function hasDispatchEvent(
  events, messageKind, reason, action, packetName, channelName, surface = 'channel'
) {
  return events.some((event) =>
    event.surface === surface &&
    event.messageKind === messageKind &&
    event.outcome === 'failed' &&
    event.reason === reason &&
    event.action === action &&
    event.packetName === packetName &&
    event.channelName === channelName
  );
}

async function publishUntilSubscribed(fanout, subscriber, received, _topic, packetName, payload) {
  const deadline = Date.now() + 1000;
  while (Date.now() < deadline) {
    await fanout.publish('events', typedPacket(packetName, payload)).submit();
    try {
      if (subscriber.subscribe(received)) {
        return;
      }
    } catch (error) {
      if (!(error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData)) {
        throw error;
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.fail('fanout pub/sub socket proof did not receive a publish before timeout');
}

async function reservePort() {
  for (;;) {
    const server = net.createServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const { port } = server.address();
    await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
    if (reservedPorts.has(port)) continue;
    reservedPorts.add(port);
    return port;
  }
}

async function reserveHeldPorts(count) {
  const entries = [];
  for (let index = 0; index < count; index += 1) {
    const server = net.createServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const { port } = server.address();
    entries.push({ port, server, released: false });
  }
  return {
    ports: entries.map((entry) => entry.port),
    async release(endpoint) {
      const port = Number(endpoint.replace(/^tcp:\/\/127\.0\.0\.1:/, ''));
      const entry = entries.find((candidate) => candidate.port === port);
      if (entry !== undefined) {
        await releaseHeldPort(entry);
      }
    },
    async releaseAll() {
      await Promise.all(entries.map((entry) => releaseHeldPort(entry)));
    }
  };
}

async function releaseHeldPort(entry) {
  if (entry.released) {
    return;
  }
  entry.released = true;
  await new Promise((resolve, reject) => entry.server.close((error) => error ? reject(error) : resolve()));
}

function subscribeMaybe(socket, received) {
  try {
    return socket.subscribe(received, zlink.RecvFlags.DontWait);
  } catch (error) {
    if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) {
      return false;
    }
    throw error;
  }
}

function decodeDotnetEnvelope(parts) {
  assert.equal(parts.length, 2);
  return {
    header: JSON.parse(parts[0].data().toString()),
    body: JSON.parse(parts[1].data().toString())
  };
}

function encodeDotnetEnvelope(header, body) {
  return [
    Buffer.from(JSON.stringify({
      formatMarker: 0xf2,
      flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
      flowOrigin: 1,
      ...header
    })),
    Buffer.from(JSON.stringify(body))
  ];
}

function appendMultipart(operation, parts) {
  let current = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index++) {
    current = current.message(parts[index]);
  }
  return current;
}

function submitRawReplyMultipart(operation, parts) {
  appendMultipart(operation, parts).submit();
}

async function submitAsyncMultipart(operation, parts) {
  await appendMultipart(operation, parts).submit();
}

function noDispatchErrorReporter() {
  return new framework.ZLinkDispatchErrorReporter(
    undefined,
    undefined,
    { reportRuntimeTaskException() {} }
  );
}

function dispatchErrorReporter(_legacyObserverType, providerResolverOrSink, maybeSink) {
  const sink = maybeSink ?? providerResolverOrSink;
  return new framework.ZLinkDispatchErrorReporter(
    undefined,
    undefined,
    sink,
    {
      diagnostics: {
        messageFlow: 'errors',
        sampleRate: 1,
        includeMessageSizes: false
      },
      liveMode: { mode: 'errors' }
    }
  );
}

function captureMultipart(parts) {
  return {
    message(part) {
      parts.push(fakeMessagePart(part));
      return this;
    },
    submit() {}
  };
}

function captureRawMultipart(parts, submit = async () => undefined) {
  return {
    message(part) {
      parts.push(part);
      return this;
    },
    submit
  };
}

function spotForwardRecord(label, submit) {
  return {
    closed: false,
    parts: [fakeMessagePart(Buffer.from(label))],
    routingId: 'source-node',
    spotId: `spot-${label}`,
    replyToken: null,
    send() {
      return captureRawMultipart([], submit);
    },
    close() {
      this.closed = true;
    }
  };
}

function submitRequestMultipart(operation, parts) {
  let current = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index++) {
    current = current.message(parts[index]);
  }
  return current.timeout(1000).submit();
}

function withTimeout(promise, timeoutMs, label) {
  let timeout;
  const guard = new Promise((_, reject) => {
    timeout = setTimeout(() => reject(new Error(`${label} timed out`)), timeoutMs);
  });
  return Promise.race([promise, guard]).finally(() => clearTimeout(timeout));
}

function withAbortTimeout(action, timeoutMs, label) {
  const controller = new AbortController();
  let timeout;
  const guard = new Promise((_, reject) => {
    timeout = setTimeout(() => {
      controller.abort();
      reject(new Error(`${label} timed out`));
    }, timeoutMs);
  });
  return Promise.race([action(controller.signal), guard]).finally(() => {
    clearTimeout(timeout);
    controller.abort();
  });
}

async function assertAborted(action) {
  await assert.rejects(
    action,
    (error) => error instanceof Error && error.message === 'The operation was aborted.'
  );
}

function assertAbortedSync(action) {
  assert.throws(
    action,
    (error) => error instanceof Error && error.message === 'The operation was aborted.'
  );
}

function createMultipartSubmitOperation() {
  return {
    message() {
      return this;
    },
    async submit() {}
  };
}

function createMultipartRequestOperation(overrides = {}) {
  return {
    message() {
      return this;
    },
    timeout() {
      return this;
    },
    async submit() { return []; },
    ...overrides
  };
}

function fakeRuntimeBackendAdapterFactory(calls, router) {
  return {
    createChannelAdapter() {
      return {
        createContext() {
          calls.push('context:create');
          return {
            nativeInstance: {},
            configureCoreHwm() {},
            getCoreHwmBudgetSnapshot() { return undefined; },
            resetCoreHwmBudgetMetrics() {},
            shutdown() {
              calls.push('context:shutdown');
            },
            async dispose() {
              calls.push('context:dispose');
            }
          };
        },
        createRouterSocket() {
          return router;
        },
        createReadablePoller() {
          return readyPoller();
        }
      };
    },
    createMonitoringAdapter() {
      return {
        openSocketMonitor() {
          return {
            nativeInstance: {},
            onEvent() {},
            recv() {
              return null;
            },
            async dispose() {}
          };
        }
      };
    }
  };
}

function fakeRuntimeRouter(calls, received) {
  let nextReceived = received;
  return {
    nativeInstance: {},
    peerWeight: 0,
    sendHighWaterMark: 0,
    receiveHighWaterMark: 0,
    sendTimeoutMs: 0,
    maxMessageSize: 0,
    disposed: false,
    setReceiveFlowState() {},
    setChannelName(channelName) {
      calls.push(`router:setChannelName:${channelName}`);
    },
    setRoutingId(routingId) {
      calls.push(`router:setRoutingId:${routingId}`);
    },
    bind(endpoint) {
      calls.push(`router:bind:${endpoint}`);
    },
    connect(endpoint) {
      calls.push(`router:connect:${endpoint}`);
    },
    disconnect(endpoint) {
      calls.push(`router:disconnect:${endpoint}`);
    },
    recv() {
      const current = nextReceived;
      nextReceived = undefined;
      return current;
    },
    send() {
      return true;
    },
    request() {
      return true;
    },
    sendToSpot() {
      return true;
    },
    requestToSpot() {
      return true;
    },
    reply() {
      if (this.disposed) {
        throw new Error('router replied after dispose');
      }
      calls.push('router:reply');
      return captureMultipart([]);
    },
    async dispose() {
      this.disposed = true;
      calls.push('router:dispose');
    }
  };
}

function fakeContext() {
  return {
    nativeInstance: {},
    shutdown() {},
    async dispose() {}
  };
}

async function waitUntil(predicate, timeoutMs = 1000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  throw new Error('Timed out waiting for condition.');
}

function fakeChannelAdapter({ dealer, router }) {
  return {
    createDealerSocket() {
      return dealer;
    },
    createPublisherSocket() {
      throw new Error('publisher not used');
    },
    createRouterSocket() {
      if (router === undefined) {
        throw new Error('router not used');
      }
      return router;
    },
    createReadablePoller() {
      return readyPoller();
    }
  };
}

function readyPoller() {
  return {
    wait() { return true; },
    dispose() {}
  };
}

function fakeBackpressuredDealer() {
  return {
    nativeInstance: {},
    sendHighWaterMark: 1000,
    sendTimeoutMs: -1,
    writable: false,
    sendAttempts: 0,
    requestAttempts: 0,
    sentParts: undefined,
    replyParts: undefined,
    setReceiveFlowState() {},
    setChannelName(channelName) {
      this.channelName = channelName;
    },
    connect(endpoint) {
      this.endpoint = endpoint;
    },
    async send(parts) {
      this.sendAttempts++;
      if (!this.writable) {
        throw new Error('dealer is not connected');
      }
      this.sentParts = parts.map(fakeMessagePart);
    },
    async request(parts) {
      this.requestAttempts++;
      if (!this.writable) {
        throw new Error('dealer is not connected');
      }
      void parts;
      return this.replyParts;
    },
    async dispose() {}
  };
}

function fakeRouteRouter(options = {}) {
  return {
    nativeInstance: {},
    sendHighWaterMark: 1000,
    sendTimeoutMs: -1,
    writable: true,
    requestAttempts: 0,
    recvAttempts: 0,
    recvQueue: [],
    setReceiveFlowState() {},
    setChannelName(channelName) {
      this.channelName = channelName;
    },
    setRoutingId(routingId) {
      this.routingId = routingId;
    },
    bind(endpoint) {
      this.endpoint = endpoint;
    },
    connect(endpoint) {
      this.connectedEndpoint = endpoint;
    },
    attachDiscovery(discovery) {
      this.discovery = discovery;
    },
    request(_targetNodeRid, _parts, timeoutMs) {
      this.requestAttempts++;
      return options.acceptWithoutReply === true
        ? new Promise((_, reject) => setTimeout(
            () => reject(new Error('binding request timed out')),
            timeoutMs ?? 30
          ))
        : Promise.reject(new Error('Route channel is not connected'));
    },
    recv() {
      this.recvAttempts++;
      return this.recvQueue.shift();
    },
    async dispose() {}
  };
}

function fakeMessagePart(part) {
  const payload = Buffer.from(part);
  return {
    data() {
      return payload;
    },
    close() {}
  };
}
