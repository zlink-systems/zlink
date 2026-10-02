const assert = require('node:assert/strict');
const test = require('node:test');
const api = require('../dist');

test('browser connect classifies close and error separately from timeout', async () => {
  const original = globalThis.WebSocket;
  try {
    for (const event of ['close', 'error', 'timeout', 'open']) {
      let socket;
      globalThis.WebSocket = class extends EventTarget {
        constructor() {
          super();
          socket = this;
          if (event !== 'timeout') queueMicrotask(() => this.dispatchEvent(new Event(event)));
        }
        close() { this.closed = true; }
      };
      const connector = api.zlinkStreamConnectorFactory.create({
        endpoint: 'ws://connector.invalid',
        connectTimeoutMs: 20,
        heartbeat: { enabled: false },
        reconnect: { enabled: false }
      });
      try {
        if (event === 'open') {
          await connector.connect();
          assert.equal(connector.state, api.ZlinkStreamConnectionState.Connected);
        } else {
          await assert.rejects(connector.connect(), error => {
            assert.equal(error.error.code, event === 'timeout'
              ? api.ZlinkStreamErrorCode.ConnectTimeout : api.ZlinkStreamErrorCode.Disconnected);
            return true;
          });
          assert.equal(connector.state, api.ZlinkStreamConnectionState.Disconnected);
          assert.equal(socket.closed, true);
        }
      } finally {
        await connector.close();
      }
    }
  } finally {
    globalThis.WebSocket = original;
  }
});
