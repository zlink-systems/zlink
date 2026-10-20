const assert = require('node:assert/strict');
const test = require('node:test');
const { badPorts } = require('undici/lib/web/fetch/constants');
const { ZLINK_FRAMEWORK_RUNTIME } = require('../../packages/nestjs/dist');
const { createBrowserSafeStreamApp } = require('./helpers/browser-safe-stream-app');

test('STREAM fixture closes each forbidden OS port before selecting another listener', async () => {
  const safePort = '41000';
  const ports = [...badPorts, safePort];
  const events = [];
  const apps = [];
  const selected = await createBrowserSafeStreamApp(async () => {
    const port = ports.shift();
    assert.notEqual(port, undefined, 'must accept the first allowed listener');
    events.push(`create:${port}`);
    const app = {
      get(token) {
        assert.equal(token, ZLINK_FRAMEWORK_RUNTIME);
        return {
          getListenerStatus(kind, name) {
            assert.equal(kind, 'stream');
            assert.equal(name, 'clients');
            return { endpoint: `ws://127.0.0.1:${port}` };
          }
        };
      },
      async close() {
        await Promise.resolve();
        events.push(`close:${port}`);
      }
    };
    apps.push(app);
    return app;
  }, 'clients');

  assert.equal(selected, apps.at(-1));
  assert.deepEqual(events, [
    ...badPorts.flatMap((port) => [`create:${port}`, `close:${port}`]),
    `create:${safePort}`
  ]);
});

test('STREAM fixture propagates startup failure without selecting another listener', async () => {
  const failure = new Error('listener startup failed');
  let calls = 0;
  await assert.rejects(
    createBrowserSafeStreamApp(async () => {
      calls++;
      throw failure;
    }, 'clients'),
    (error) => error === failure
  );
  assert.equal(calls, 1);
});

test('STREAM fixture propagates rejected-listener close failure before selecting another', async () => {
  const failure = new Error('listener close failed');
  let calls = 0;
  await assert.rejects(
    createBrowserSafeStreamApp(async () => {
      calls++;
      return {
        get: () => ({ getListenerStatus: () => ({ endpoint: 'ws://127.0.0.1:2049' }) }),
        async close() {
          throw failure;
        }
      };
    }, 'clients'),
    (error) => error === failure
  );
  assert.equal(calls, 1);
});
