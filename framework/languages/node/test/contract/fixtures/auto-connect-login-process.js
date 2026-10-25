const net = require('node:net');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const n = require('../../../packages/nestjs/dist');
const c = require('../../../packages/stream-connector/dist');
let spots;
class Lobby {}
class SessionFactory {
  async create(context) {
    let authenticated = false;
    return {
      context,
      async onDispatch(dispatch, payload) {
        if (dispatch.packetName === 'AuthenticateReq') {
          await Promise.resolve();
          authenticated = true;
          console.log('AuthenticateReq handled');
          await context.client.reply({ authenticated: true }).submit();
        } else if (dispatch.packetName === 'OpenLobbyReq') {
          if (!authenticated) throw new Error('AuthenticateReq is required');
          const created = await spots.create('lobby').inMesh('lobby-mesh').submit();
          console.log('OpenLobbyReq public spots.create awaited', created.state);
          await context.client.reply({ opened: created.state === 'created' }).submit();
        } else throw new Error('Unexpected packet ' + dispatch.packetName);
      }
    };
  }
}
async function unusedPort() {
  const s = net.createServer();
  await new Promise((r) => s.listen(0, '127.0.0.1', r));
  const port = s.address().port;
  await new Promise((r) => s.close(r));
  return port;
}
async function main() {
  const port = await unusedPort();
  const b = n.zlinkFramework().options({ locations: { useInMemoryStores: true } });
  b.configureDispatch().messageFlow('normal');
  b.addRouteMesh('lobby-mesh')
    .listen('tcp://127.0.0.1:0')
    .routingId('lobby-node')
    .objects()
    .server()
    .addSpotFactory('lobby', Lobby, (f) => f.disableRelocation());
  b.addStreamNode('gateway').bind(`ws://127.0.0.1:${port}`).registerSession(SessionFactory);
  class App {}
  Module({ imports: [n.ZLinkModule.forRoot(b.build())], providers: [Lobby, SessionFactory] })(App);
  console.log('START', process.env.NODE_ENV);
  const app = await NestFactory.createApplicationContext(App, {
    logger: false,
    abortOnError: false
  });
  let client;
  try {
    console.log('HOST READY');
    spots = app.get(n.ZLINK_SPOT_MANAGER);
    client = c.zlinkStreamConnectorFactory.create({ endpoint: `ws://127.0.0.1:${port}` });
    await client.connect();
    const auth = await client.request({ token: 'demo' }).packetName('AuthenticateReq').submit();
    require('node:assert/strict').deepEqual(auth, { authenticated: true });
    const lobby = await client.request({}).packetName('OpenLobbyReq').submit();
    require('node:assert/strict').deepEqual(lobby, { opened: true });
    console.log('LOGIN_LOBBY_COMPLETE');
  } finally {
    if (client) await client.close();
    await app.close();
  }
}
main().catch((e) => {
  console.error(e.stack);
  process.exit(1);
});
