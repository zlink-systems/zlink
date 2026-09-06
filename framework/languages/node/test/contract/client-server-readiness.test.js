const assert = require('node:assert/strict');
const net = require('node:net');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');

const framework = require('../../packages/framework/dist');
const nestjs = require('../../packages/nestjs/dist');

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
  const port = await reservePort();
  const app = await createApp(channel => channel.client().connect(`tcp://127.0.0.1:${port}`));
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
  }
});

test('Client+Server ClientServer topology counts the local Server once after a public request', async () => {
  const app = await createApp(channel => {
    channel.client();
    channel.server().listen().setWeight(100).addRequestHandler('Ping', PingHandler);
  });
  try {
    const reply = await app.get(nestjs.ZLINK_CHANNEL_CLIENT)
      .requestToChannel('work', new Ping('local'))
      .timeout(1000)
      .submit();
    assert.deepEqual(reply, { value: 'local' });
    const runtime = app.get(nestjs.ZLINK_CLIENT_SERVER_RUNTIME);
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
  const port = await reservePort();
  const remote = await createApp(channel => {
    channel.server().listen(port).setWeight(200).addRequestHandler('Ping', PingHandler);
  });
  try {
    const local = await createApp(channel => {
      channel.client().connect(`tcp://127.0.0.1:${port}`);
      channel.server().listen().setWeight(100).addRequestHandler('Ping', PingHandler);
    });
    try {
      const runtime = local.get(nestjs.ZLINK_CLIENT_SERVER_RUNTIME);
      const deadline = Date.now() + 5000;
      while (runtime.snapshot('work').readyTargetCount !== 2 && Date.now() < deadline) {
        await new Promise(resolve => setImmediate(resolve));
      }
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

async function reservePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  return port;
}
