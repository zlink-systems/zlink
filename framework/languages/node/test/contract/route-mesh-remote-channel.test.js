'use strict';
const assert = require('node:assert/strict');
const net = require('node:net');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('@zlink-systems/framework');
const nestjs = require('@zlink-systems/nestjs');

class RemoteChannelProbe { constructor(value) { this.value = value; } }
class RemoteChannelReply { constructor(value, owner) { this.value = value; this.owner = owner; } }
framework.ZLinkPacket('RemoteChannelProbe')(RemoteChannelProbe);
framework.ZLinkPacket('RemoteChannelReply')(RemoteChannelReply);

async function endpoint() {
  const listener = net.createServer();
  await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
  const port = listener.address().port;
  await new Promise(resolve => listener.close(resolve));
  return `tcp://127.0.0.1:${port}`;
}

async function createApp(owner, listen, peer, deliveries) {
  class RequestHandler {
    handle(message) { deliveries.push(['request', message.value]); return new RemoteChannelReply(message.value, owner); }
  }
  class SendHandler { handle(message) { deliveries.push(['send', message.value]); } }
  const builder = nestjs.zlinkFramework();
  const mesh = builder.addRouteMesh('remote-only').listen(listen).routingId(owner);
  mesh.channel('remote-only').server()
    .addRequestHandler('RemoteChannelProbe', RequestHandler)
    .addSendHandler('RemoteChannelProbe', SendHandler);
  if (peer) mesh.peerConnections().connect(peer);
  class AppModule {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(builder.build())] })(AppModule);
  const app = await NestFactory.createApplicationContext(AppModule, { logger: false, abortOnError: false });
  await app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME).start();
  return app;
}

function status(app) { return app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME).snapshot('remote-only'); }
async function ready(app) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (status(app).readyPeerCount === 1) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.fail('Public RouteMesh peer did not become ready within existing 5s fixture bound');
}
async function delivered(deliveries, count) {
  const deadline = Date.now() + 5000;
  while (deliveries.length < count && Date.now() < deadline) await new Promise(resolve => setImmediate(resolve));
  assert.equal(deliveries.length, count);
}

test('public RouteMesh self-only Channel has no target and is not ready', async () => {
  const deliveries = [];
  const app = await createApp('self-only', await endpoint(), null, deliveries);
  try {
    const route = app.get(nestjs.ZLINK_ROUTE_CLIENT);
    const notFound = error => error.kind === framework.ZLinkFrameworkErrorKind.NotFound;
    await assert.rejects(route.sendToChannel('remote-only', new RemoteChannelProbe('send')).submit(), notFound);
    await assert.rejects(route.requestToChannel('remote-only', new RemoteChannelProbe('request')).timeout(1000).submit(), notFound);
    assert.deepEqual(deliveries, []);
    const channel = status(app).channels.find(value => value.channelName === 'remote-only');
    assert.equal(channel.readyTargetCount, 0);
    assert.equal(channel.isReady, false);
  } finally { await app.close(); }
});

test('public RouteMesh Channel send and request always select remote Server', async () => {
  const localDeliveries = [], remoteDeliveries = [];
  const localEndpoint = await endpoint(), remoteEndpoint = await endpoint();
  const local = await createApp('channel-local', localEndpoint, remoteEndpoint, localDeliveries);
  let remote;
  try {
    remote = await createApp('channel-remote', remoteEndpoint, localEndpoint, remoteDeliveries);
    await ready(local); await ready(remote);
    const route = local.get(nestjs.ZLINK_ROUTE_CLIENT);
    for (let index = 0; index < 4; index++) {
      await route.sendToChannel('remote-only', new RemoteChannelProbe(`send-${index}`)).submit();
      const reply = await route.requestToChannel('remote-only', new RemoteChannelProbe(`request-${index}`)).timeout(1000).submit();
      assert.equal(reply.owner, 'channel-remote');
      assert.equal(reply.value, `request-${index}`);
    }
    await delivered(remoteDeliveries, 8);
    assert.deepEqual(localDeliveries, []);
    for (const app of [local, remote]) {
      const channel = status(app).channels.find(value => value.channelName === 'remote-only');
      assert.equal(channel.readyTargetCount, 1);
      assert.equal(channel.isReady, true);
    }
  } finally { await remote?.close(); await local.close(); }
});
