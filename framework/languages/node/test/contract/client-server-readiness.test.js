const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');

const framework = require('../../packages/framework/dist');
const nestjs = require('../../packages/nestjs/dist');
const { waitForClientServerTargets } = require('./helpers/client-server-readiness');
const { holdTcpEndpoint } = require('./helpers/nestjs-test-utils');

class Ping {
  constructor(value) {
    this.value = value;
  }
}

class PingHandler {
  handle(request) {
    return { value: request.value };
  }
}

for (const weight of [100, 0]) {
  test(`Server-only ClientServer topology counts its local Ready Server with weight ${weight}`, async () => {
    const app = await createApp(channel => {
      channel.server().listen().setWeight(weight).addRequestHandler('Ping', PingHandler);
    });
    try {
      const runtime = app.get(nestjs.ZLINK_CLIENT_SERVER_RUNTIME);
      const status = runtime.snapshot('work');
      assert.equal(app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).status.state,
        framework.ZLinkFrameworkRuntimeState.Serving);
      assert.equal(status.localRole, 'server');
      assert.equal(status.state, weight > 0
        ? framework.ZLinkTopologyState.Ready
        : framework.ZLinkTopologyState.Degraded);
      assert.equal(status.isReady, weight > 0);
      assert.equal(runtime.isReady('work'), weight > 0);
      assert.equal(status.readyTargetCount, weight > 0 ? 1 : 0);
      assert.equal(status.targets.length, 1);
      assert.equal(status.targets[0].weight, weight);
      assert.equal(status.targets[0].state, framework.ZLinkPeerState.Ready);
      assert.equal(status.targets[0].unavailableReason, undefined);

      const client = app.get(nestjs.ZLINK_CHANNEL_CLIENT);
      for (const call of [
        client.sendToChannel('work', new Ping('send')),
        client.requestToChannel('work', new Ping('request'))
      ]) {
        await assert.rejects(() => call.submit(), error =>
          error instanceof framework.ZLinkFrameworkException
          && error.kind === framework.ZLinkFrameworkErrorKind.NotConfigured);
      }
    } finally {
      await app.close();
    }
  });
}

test('Client-only ClientServer topology without a Ready Server is degraded', async () => {
  // A plain listener holds the endpoint; it never completes a ZMTP handshake.
  const unreachable = await holdTcpEndpoint();
  const app = await createApp(channel => channel.client().connect(unreachable.endpoint));
  try {
    const runtime = app.get(nestjs.ZLINK_CLIENT_SERVER_RUNTIME);
    const status = runtime.snapshot('work');
    assert.equal(app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).status.state,
      framework.ZLinkFrameworkRuntimeState.Serving);
    assert.equal(status.localRole, 'client');
    assert.equal(status.state, framework.ZLinkTopologyState.Degraded);
    assert.equal(status.isReady, false);
    assert.equal(runtime.isReady('work'), false);
    assert.equal(status.readyTargetCount, 0);
    assert.deepEqual(status.targets, []);
  } finally {
    await app.close();
    await unreachable.close();
  }
});

test('Client+Server ClientServer topology counts the local Server once after a public request', async () => {
  const app = await createApp(channel => {
    channel.client();
    channel.server().listen().setWeight(100).addRequestHandler('Ping', PingHandler);
  });
  try {
    const runtime = app.get(nestjs.ZLINK_CLIENT_SERVER_RUNTIME);
    await waitForClientServerTargets(runtime, 'work', 1);
    const reply = await app.get(nestjs.ZLINK_CHANNEL_CLIENT)
      .requestToChannel('work', new Ping('local'))
      .timeout(1000)
      .submit();
    assert.deepEqual(reply, { value: 'local' });
    const status = runtime.snapshot('work');
    assert.equal(app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).status.state,
      framework.ZLinkFrameworkRuntimeState.Serving);
    assert.equal(status.localRole, 'clientAndServer');
    assert.equal(status.state, framework.ZLinkTopologyState.Ready);
    assert.equal(status.isReady, true);
    assert.equal(runtime.isReady('work'), true);
    assert.equal(status.readyTargetCount, 1);
    assert.equal(status.targets.length, 1);
    assert.equal(status.targets[0].weight, 100);
    assert.equal(status.targets[0].state, framework.ZLinkPeerState.Ready);
  } finally {
    await app.close();
  }
});

test('ClientServer topology counts distinct local and remote Ready Servers together', async () => {
  const remote = await createApp(channel => {
    channel.server().listen(0).setWeight(200).addRequestHandler('Ping', PingHandler);
  });
  try {
    const endpoint = remote.get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
      .getListenerStatus('clientServer', 'work').endpoint;
    const local = await createApp(channel => {
      channel.client().connect(endpoint);
      channel.server().listen().setWeight(100).addRequestHandler('Ping', PingHandler);
    });
    try {
      const runtime = local.get(nestjs.ZLINK_CLIENT_SERVER_RUNTIME);
      await waitForClientServerTargets(runtime, 'work', 2);
      const status = runtime.snapshot('work');
      assert.equal(status.state, framework.ZLinkTopologyState.Ready);
      assert.equal(status.isReady, true);
      assert.equal(runtime.isReady('work'), true);
      assert.equal(status.readyTargetCount, 2);
      assert.equal(status.targets.length, 2);
      assert.notEqual(status.targets[0].nodeRid, status.targets[1].nodeRid);
      assert.deepEqual(status.targets.map(target => target.weight).sort((a, b) => a - b), [100, 200]);
      assert.ok(status.targets.every(target => target.state === framework.ZLinkPeerState.Ready));
    } finally {
      await local.close();
    }
  } finally {
    await remote.close();
  }
});

test('Disconnected ClientServer target reports not_connected and no_ready_target', async () => {
  const remote = await createApp(channel => {
    channel.server().listen(0).addRequestHandler('Ping', PingHandler);
  });
  let local;
  try {
    const endpoint = remote.get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
      .getListenerStatus('clientServer', 'work').endpoint;
    local = await createApp(channel => channel.client().connect(endpoint));
    const runtime = local.get(nestjs.ZLINK_CLIENT_SERVER_RUNTIME);
    await waitForClientServerTargets(runtime, 'work', 1);
    const ready = runtime.snapshot('work').targets[0];
    await remote.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).shutdown();
    await waitForClientServerTargets(runtime, 'work', 0);
    const target = runtime.snapshot('work').targets.find(value => value.nodeRid === ready.nodeRid);
    assert.ok(target);
    assert.equal(target.state, framework.ZLinkPeerState.NotConnected);
    assert.equal(target.unavailableReason, framework.ZLinkTopologyReason.NoReadyTarget);
  } finally {
    if (local !== undefined) await local.close();
    await remote.close();
  }
});

function createApp(configure) {
  const builder = nestjs.zlinkFramework();
  builder.configureDispatch().messageFlow('normal');
  configure(builder.addClientServerChannel('work'));
  class AppModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(builder.build())],
    providers: [PingHandler]
  })(AppModule);
  return NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false });
}

test('local Server descriptor changes publish without physical Client events or observers', async () => {
  const internal = require('../../packages/framework/dist/internal');
  const { ZLinkChannelSocketRegistry } = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
  const registration = internal.createFrameworkRegistration({ channels: { work: { server: { bind: 'tcp://127.0.0.1:0' }, sendHandlers: [{ packetName: 'notice', handler: { handle() {} } }] } } });
  const sockets = new ZLinkChannelSocketRegistry(registration, {}, {});
  const descriptor = { channelName: 'work', serverRid: 'local', lifecycleGeneration: 1n, weight: 100, state: framework.ZLinkFrameworkRuntimeState.Serving };
  sockets.setClientServerServerDescriptor(descriptor, 'work');
  const manager = {
    clientServerTopology: name => ({ localRole: 'server', descriptors: sockets.clientServerActiveTargets(name) }),
    observeClientServerTopology(name, callback) {
      const monitor = sockets.clientServerMonitoringSource(name);
      monitor.onChange(callback);
      return () => { void monitor.dispose(); };
    }
  };
  let physicalEvents = 0;
  const physicalMonitor = sockets.clientServerMonitoringSource('work');
  physicalMonitor.onEvent(() => { physicalEvents += 1; });
  const runtime = new internal.ZLinkClientServerRuntimeProjection(() => manager);
  assert.equal(runtime.snapshot('work').sequence, 1n);
  sockets.setClientServerServerDescriptor({ ...descriptor, state: framework.ZLinkFrameworkRuntimeState.Draining }, 'work');
  sockets.setClientServerServerDescriptor(descriptor, 'work');
  assert.equal(runtime.snapshot('work').sequence, 3n);
  const events = runtime.observe('work')[Symbol.asyncIterator]();
  assert.equal((await events.next()).value.status.sequence, 3n);
  sockets.setClientServerServerDescriptor({ ...descriptor, state: framework.ZLinkFrameworkRuntimeState.Draining }, 'work');
  const draining = (await events.next()).value.status;
  assert.equal(draining.sequence, 4n);
  assert.equal(draining.targets[0].state, framework.ZLinkPeerState.Draining);
  sockets.setClientServerServerDescriptor(undefined, 'work');
  const removed = (await events.next()).value.status;
  assert.equal(removed.sequence, 5n);
  assert.equal(removed.targets.length, 0);
  assert.equal(physicalEvents, 0);
  await events.return();
  runtime.stopObservers();
  await physicalMonitor.dispose();
});
