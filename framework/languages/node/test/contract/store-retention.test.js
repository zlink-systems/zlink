const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkInMemoryProviderLocationStore } = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const { ZLinkRedisLocationStore } = require('../../packages/framework-locations-redis/dist/opaque-store');
const { ZLinkRedisRelocationStore } = require('../../packages/framework-locations-redis/dist/relocation-store');

test('Store providers round 1.5 ms retention up to 2 ms', async () => {
  const now = new Date(1000);
  const key = { value: 'fractional-retention' };
  const memory = new ZLinkInMemoryProviderLocationStore(() => now);
  await memory.write({
    conditions: [],
    mutations: [{ kind: 'put', key, bytes: Uint8Array.of(1), retentionMs: 1.5 }]
  });
  assert.equal((await memory.read(key)).value.expiresAt.getTime(), 1002);

  const locationClient = {
    isReady: true,
    async sendCommand(args) {
      assert.equal(JSON.parse(args[11])[0][3], 2);
      return ['applied', '1000'];
    }
  };
  const location = new ZLinkRedisLocationStore({ client: locationClient, keyPrefix: 'rounding' });
  try {
    await location.write({
      conditions: [],
      mutations: [{ kind: 'put', key, bytes: Uint8Array.of(1), retentionMs: 1.5 }]
    });
  } finally {
    await location.dispose();
  }

  const relocationClient = {
    isReady: true,
    async sendCommand(args) {
      assert.equal(args[5], '2');
      return ['stored', '1000', '1002'];
    }
  };
  const relocation = new ZLinkRedisRelocationStore({ client: relocationClient, keyPrefix: 'rounding' });
  try {
    assert.equal(
      (await relocation.put({ value: 'fractional' }, Uint8Array.of(1), 1.5)).expiresAt.getTime(),
      1002
    );
  } finally {
    await relocation.dispose();
  }
});
