const assert = require('node:assert/strict');
const net = require('node:net');
const test = require('node:test');
const { once } = require('node:events');

const framework = require('../../packages/framework/dist/internal');

test('RID-fenced admission warning reports rejection reason, intended endpoint, and advertised endpoint', async () => {
  const localEndpoint = 'tcp://127.0.0.1:0';
  const remoteEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const advertisedEndpoint = remoteEndpoint.replace('127.0.0.1', '127.0.0.2');
  const target = createRuntime('node-b', remoteEndpoint, '127.0.0.2');
  const caller = createRuntime('node-a', localEndpoint, undefined, mesh => {
    mesh.peerConnections().connect('node-b', remoteEndpoint);
  });
  const warnings = [];
  const originalWarn = console.warn;
  console.warn = (...values) => warnings.push(values.join(' '));

  try {
    await target.start();
    await caller.start();
    await waitUntil(() => warnings.some(message =>
      message.includes('invalidDescriptor') &&
      message.includes(`expected='${remoteEndpoint}'`) &&
      message.includes(`actual='${advertisedEndpoint}'`)
    ));
  } finally {
    console.warn = originalWarn;
    await caller.stop();
    await target.stop();
  }
});

function createRuntime(routingId, bind, advertiseHost, configure = () => {}) {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions(builder => {
      const mesh = builder.addRouteMesh('mesh').listen(bind).routingId(routingId);
      if (advertiseHost !== undefined) mesh.setAdvertiseHost(advertiseHost);
      configure(mesh);
    })
  );
  return new framework.ZLinkFrameworkRuntimeHost({ registration });
}

async function reservePort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const port = server.address().port;
  await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  return port;
}

async function waitUntil(predicate) {
  const deadline = Date.now() + 10_000;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.fail('Admission warning did not become observable before the deadline.');
}
