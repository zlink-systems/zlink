const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const test = require('node:test');
const { once } = require('node:events');
const zlink = require('@zlink-systems/zlink');

const packageRoot = path.dirname(path.dirname(require.resolve('@zlink-systems/zlink')));

test('loaded node binding package provides a native artifact for the current platform', () => {
  const artifact = activeNativeArtifact();

  assert.ok(artifact, 'expected a native addon artifact for the current platform');
});

test('loaded node binding monitor callback carries a STREAM peer routing id', async () => {
  const port = await reservePort();
  const context = zlink.createContext();
  const stream = zlink.createStreamSocket(context);
  const monitor = stream.monitorOpen([
    zlink.MonitorEventType.Accepted,
    zlink.MonitorEventType.Disconnected
  ]);
  const client = new net.Socket();
  let disconnected;
  monitor.onEvent((event) => {
    if (event.event === zlink.MonitorEventType.Disconnected) {
      disconnected = event;
    }
  });

  try {
    stream.bind(`tcp://127.0.0.1:${port}`);
    await new Promise((resolve, reject) => {
      client.once('error', reject);
      client.connect(port, '127.0.0.1', resolve);
    });
    client.write(Buffer.from('installed-native-monitor-probe'));
    const received = new zlink.Received();
    assert.ok(await waitFor(() => stream.recv(received, zlink.RecvFlags.DontWait)));
    received.close();
    client.end();
    await once(client, 'close');

    const event = await waitFor(() => disconnected);
    assert.ok(event, 'expected the installed native addon to deliver the disconnect callback');
    assert.ok(event.routingId, 'expected the disconnect callback to identify the STREAM peer');
    assert.ok(event.routingId.size > 0);
  } finally {
    client.destroy();
    monitor.close();
    stream.close();
    context.close();
  }
});

function activeNativeArtifact() {
  const candidates = [
    path.join(packageRoot, 'build', 'Release', 'zlink.node'),
    path.join(packageRoot, 'prebuilds', `${process.platform}-${process.arch}`, 'zlink.node')
  ];
  for (const file of candidates) {
    if (fs.existsSync(file)) {
      return file;
    }
  }
  return null;
}

async function reservePort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { port } = server.address();
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function waitFor(read) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const value = read();
    if (value) return value;
    await new Promise((resolve) => setImmediate(resolve));
  }
  return undefined;
}
