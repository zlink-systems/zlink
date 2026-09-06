const assert = require('node:assert/strict');
const test = require('node:test');
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
        assert.equal(args[0], 'EVAL');
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
