const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

class StreamSession {}
class RequestHandler {
  handle() {
    return {};
  }
}

test('Framework runtime reports each bound listener through one status API', async () => {
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      spotNodes: {
        mesh: { router: { routingId: 'listener-node', bind: 'tcp://127.0.0.1:0' } }
      },
      channels: {
        requests: {
          server: { bind: 'tcp://127.0.0.1:0' },
          requestHandlers: [{ packetName: 'Request', handlerType: RequestHandler }]
        },
        events: { publisher: { bind: 'tcp://127.0.0.1:0' } }
      },
      streamNodes: {
        gateway: { bind: 'tcp://127.0.0.1:0', session: StreamSession }
      }
    })
  });

  for (const [kind, name] of [
    ['routeMesh', 'mesh'],
    ['clientServer', 'requests'],
    ['fanout', 'events'],
    ['stream', 'gateway']
  ]) {
    assert.throws(
      () => runtime.getListenerStatus(kind, name),
      (error) => error instanceof framework.ZLinkConfigurationException
    );
  }
  assert.throws(
    () => runtime.getListenerStatus('fanout', 'missing'),
    (error) => error instanceof framework.ZLinkConfigurationException
  );

  try {
    await runtime.start();
    for (const [kind, name] of [
      ['routeMesh', 'mesh'],
      ['clientServer', 'requests'],
      ['fanout', 'events'],
      ['stream', 'gateway']
    ]) {
      const status = runtime.getListenerStatus(kind, name);
      assert.equal(status.kind, kind);
      assert.equal(status.name, name);
      assert.match(status.endpoint, /^tcp:\/\/127\.0\.0\.1:[1-9]\d*$/);
      assert.ok(status.observedAt instanceof Date);
    }
    assert.throws(
      () => runtime.getListenerStatus('routeMesh', 'missing'),
      (error) => error instanceof framework.ZLinkConfigurationException
    );
  } finally {
    await runtime.stop();
  }
});
