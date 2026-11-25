const assert = require('node:assert/strict');
const test = require('node:test');
const { SimpleError } = require('redis');
const { ZLinkRedisRelocationStore } = require('../../packages/framework-locations-redis/dist');

for (const envelopeBytes of [23, 24]) {
  test(`Redis relocation encoded blob of 64 MiB + ${envelopeBytes} bytes is ${envelopeBytes === 23 ? 'accepted' : 'rejected before I/O'}`, async () => {
    const payload = Buffer.alloc(64 * 1024 * 1024 + envelopeBytes);
    payload[0] = 0xff;
    payload[payload.length - 1] = 0x80;
    const reference = { value: 'boundary-blob' };
    const storeNow = 1_700_000_000_000;
    const retentionMs = 60_000;
    let commands = 0;
    const client = {
      get isReady() {
        assert.equal(envelopeBytes, 23, 'oversized input must be rejected before accessing Redis');
        return true;
      },
      async sendCommand(args) {
        commands += 1;
        assert.equal(args[0], 'EVALSHA');
        assert.match(args[1], /^[0-9a-f]{40}$/);
        assert.equal(args[2], '1');
        assert.equal(args[3], 'blob-bound:{zlink-relocation-v1}:blob:boundary-blob');
        assert.equal(args[4].byteLength, payload.byteLength);
        assert.equal(Buffer.compare(args[4], payload), 0);
        assert.equal(args[5], String(retentionMs));
        return ['stored', String(storeNow), String(storeNow + retentionMs)];
      }
    };
    const store = new ZLinkRedisRelocationStore({ client, keyPrefix: 'blob-bound' });
    try {
      if (envelopeBytes === 23) {
        assert.deepEqual(await store.put(reference, payload, retentionMs), {
          kind: 'stored',
          storeNow: new Date(storeNow),
          expiresAt: new Date(storeNow + retentionMs)
        });
        assert.equal(commands, 1);
      } else {
        await assert.rejects(
          () => store.put(reference, payload, retentionMs),
          RangeError
        );
        assert.equal(commands, 0);
      }
    } finally {
      await store.dispose();
    }
  });
}

test('Redis relocation forwards non-NOSCRIPT errors without submitting EVAL', async () => {
  for (const error of [
    new SimpleError('ERR script failed'),
    new SimpleError('NOSCRIPTING script error'),
    new Error('NOSCRIPT transport failure')
  ]) {
    const commands = [];
    const client = {
      isReady: true,
      async sendCommand([command]) {
        commands.push(command);
        throw error;
      }
    };
    const store = new ZLinkRedisRelocationStore({ client, keyPrefix: 'script-errors' });
    try {
      await assert.rejects(store.read({ value: 'subject' }), actual => actual === error);
      assert.deepEqual(commands, ['EVALSHA']);
    } finally {
      await store.dispose();
    }
  }
});

test('Redis relocation does not submit EVAL after a timed-out EVALSHA', async () => {
  const commands = [];
  let rejectCommand;
  const client = {
    isReady: true,
    sendCommand([command]) {
      commands.push(command);
      return new Promise((_resolve, reject) => { rejectCommand = reject; });
    }
  };
  const store = new ZLinkRedisRelocationStore({ client, keyPrefix: 'script-timeout', operationTimeoutMs: 10 });
  try {
    await assert.rejects(store.read({ value: 'subject' }), /timed out after 10 ms/);
    rejectCommand(new SimpleError('NOSCRIPT No matching script. Please use EVAL.'));
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(commands, ['EVALSHA']);
  } finally {
    await store.dispose();
  }
});

test('Redis relocation does not submit EVAL after a canceled EVALSHA', async () => {
  const commands = [];
  const controller = new AbortController();
  const reason = new Error('Canceled by the caller');
  const client = {
    isReady: true,
    async sendCommand([command]) {
      commands.push(command);
      controller.abort(reason);
      throw new SimpleError('NOSCRIPT No matching script. Please use EVAL.');
    }
  };
  const store = new ZLinkRedisRelocationStore({ client, keyPrefix: 'script-cancellation' });
  try {
    await assert.rejects(store.read({ value: 'subject' }, controller.signal), error => error === reason);
    assert.deepEqual(commands, ['EVALSHA']);
  } finally {
    await store.dispose();
  }
});
