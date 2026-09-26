const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

test('RID-fenced admission warning reports rejection reason, intended endpoint, and advertised endpoint', async () => {
  // Each RouteMesh binds an OS-assigned port. The target advertises the port
  // it bound under another host; the caller connects to it on loopback.
  const target = createRuntime('node-b', 'tcp://127.0.0.1:*', '127.0.0.2');
  let caller;
  const warnings = [];
  const originalWarn = console.warn;
  console.warn = (...values) => warnings.push(values.join(' '));

  try {
    await target.start();
    const advertisedEndpoint = target.requirePrimaryMeshNode().status().localEndpoint;
    assert.match(advertisedEndpoint, /^tcp:\/\/127\.0\.0\.2:\d+$/);
    const remoteEndpoint = advertisedEndpoint.replace('127.0.0.2', '127.0.0.1');
    caller = createRuntime('node-a', 'tcp://127.0.0.1:*', undefined, mesh => {
      mesh.peerConnections().connect('node-b', remoteEndpoint);
    });
    await caller.start();
    await waitUntil(() => warnings.some(message =>
      message.includes('invalidDescriptor') &&
      message.includes(`expected='${remoteEndpoint}'`) &&
      message.includes(`actual='${advertisedEndpoint}'`)
    ));
  } finally {
    console.warn = originalWarn;
    await caller?.stop();
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

async function waitUntil(predicate) {
  const deadline = Date.now() + 10_000;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.fail('Admission warning did not become observable before the deadline.');
}
