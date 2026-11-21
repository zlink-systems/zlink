const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
const {
  keys,
  encodeClock
} = require('../../packages/framework-locations-etcd/dist/internal/encoding');
const { ZLinkEtcdLocationStore } = require('../../packages/framework-locations-etcd/dist');

// The fixture must require both client certificates and etcd user authentication.
const configPath = process.env.ZLINK_ETCD_SECURITY_CONFIG;
const options =
  configPath === undefined ? undefined : JSON.parse(fs.readFileSync(configPath, 'utf8'));

test(
  'etcd live mTLS and user authentication support provider reads and revision-pinned keys_only',
  {
    skip:
      options === undefined
        ? 'Set ZLINK_ETCD_SECURITY_CONFIG for a secured etcd v3.6.16 fixture'
        : false
  },
  async () => {
    const client = new EtcdClient(options);
    const store = new ZLinkEtcdLocationStore(options);
    const key = Buffer.from(`${options.namespace}/keys-only`);
    const value = Buffer.from([0, 1, 127, 128, 255]);
    const rpc = (method, request) => client.rpc('KV', method, request, client.operation());
    try {
      const written = await rpc('put', { key, value });
      await rpc('put', { key, value: Buffer.from('new value') });
      const request = { key, revision: written.header.revision, serializable: false };
      const full = await rpc('range', { ...request, keys_only: false });
      const only = await rpc('range', { ...request, keys_only: true });
      assert.equal(full.kvs.length, 1);
      assert.equal(only.kvs.length, 1);
      assert.deepEqual(full.kvs[0].value, value);
      assert.equal(only.kvs[0].value.length, 0);
      for (const field of ['key', 'create_revision', 'mod_revision', 'version']) {
        assert.deepEqual(only.kvs[0][field], full.kvs[0][field], field);
      }
      assert.equal(full.kvs[0].mod_revision, written.header.revision);
      assert.equal(only.header.cluster_id, full.header.cluster_id);
      const latest = await rpc('range', { key });
      assert.deepEqual(latest.kvs[0].value, Buffer.from('new value'));
      assert.notEqual(latest.kvs[0].mod_revision, full.kvs[0].mod_revision);

      await rpc('put', { key: keys(options.namespace).clock, value: encodeClock(1000n) });
      const result = await store.read({ value: 'absent' });
      assert.equal(result.kind, 'missing');
      assert.equal(result.storeNow.getTime(), 1000);
    } finally {
      await store.dispose();
      client.close();
    }
  }
);

for (const failure of [
  'wrong password',
  'missing credentials',
  'wrong CA',
  'wrong hostname',
  'missing client identity'
]) {
  test(
    `etcd live security rejects ${failure}`,
    {
      skip:
        options === undefined
          ? 'Set ZLINK_ETCD_SECURITY_CONFIG for a secured etcd v3.6.16 fixture'
          : false
    },
    async () => {
      const invalid = structuredClone(options);
      if (failure === 'wrong password') invalid.credentials.password += '-wrong';
      if (failure === 'missing credentials') delete invalid.credentials;
      if (failure === 'wrong CA')
        invalid.tls.trustedCaPem = fs.readFileSync(process.env.ZLINK_ETCD_WRONG_CA, 'utf8');
      if (failure === 'wrong hostname')
        invalid.endpoints = invalid.endpoints.map((endpoint) =>
          endpoint.replace('localhost', '127.0.0.1')
        );
      if (failure === 'missing client identity') delete invalid.tls.clientIdentity;
      const client = new EtcdClient(invalid);
      try {
        await assert.rejects(
          client.rpc('KV', 'range', { key: Buffer.from('security-probe') }, client.operation()),
          (error) => error.kind === 'storeFailure'
        );
      } finally {
        client.close();
      }
    }
  );
}
