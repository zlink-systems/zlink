const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkHostSpotAddressTransport
} = require('../../packages/framework/dist/runtime/host/spot-address-transport');
const {
  ZLinkAuthoritySpotRouteResolver
} = require('../../packages/framework/dist/runtime/locations/resolvers');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/contracts/Errors');
const {
  DefaultZLinkSpotOutbound
} = require('../../packages/framework/dist/runtime/spots/spot-outbound');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

for (const operation of ['send', 'request']) {
  test(`cache miss Spot ${operation} classifies provider read timeout as Unavailable`, async () => {
    const providerFailure = new Error('private Redis command/key details');
    providerFailure.name = 'TimeoutError';
    let reads = 0;
    const store = new ZLinkLocationStoreRepository({
      async read() {
        reads++;
        throw providerFailure;
      }
    });
    const resolver = new ZLinkAuthoritySpotRouteResolver(store, () => 'mesh');
    const transport = new ZLinkHostSpotAddressTransport({
      resolver: () => resolver,
      routed: {},
      meshNames: () => ['mesh'],
      meshNode: () => undefined,
      completions: () => undefined,
      defaultRequestTimeoutMs: 60_000
    });
    const outbound = new DefaultZLinkSpotOutbound({
      serial: new ZLinkSpotSerialTurnExecutor(),
      addressTransport: transport
    });
    class Packet {}
    const pending =
      operation === 'send'
        ? outbound.sendToSpot('uncached-spot', new Packet()).submit()
        : outbound.requestToSpot('uncached-spot', new Packet()).timeout(60_000).submit();
    await assert.rejects(pending, (error) => {
      assert.ok(error instanceof ZLinkFrameworkException);
      assert.equal(error.kind, ZLinkFrameworkErrorKind.Unavailable);
      assert.equal(error.cause, providerFailure);
      assert.ok(!error.message.includes('Redis'));
      return true;
    });
    assert.equal(reads, 1);
  });
}

const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

for (const operation of ['read', 'write', 'scan']) {
  test(`repository classifies ${operation} provider failure and retains its cause`, async () => {
    const inner = new ZLinkInMemoryProviderLocationStore();
    const cause = new Error('private provider details');
    const provider = Object.fromEntries(
      ['read', 'write', 'scan'].map((name) => [
        name,
        (...args) => {
          if (name === operation) throw cause;
          return inner[name](...args);
        }
      ])
    );
    const repository = new ZLinkLocationStoreRepository(provider);
    const pending =
      operation === 'read'
        ? repository.readAuthority(encodeAuthorityKey('user_spot', 'spot'))
        : operation === 'write'
          ? repository.claimOwnerLease('owner', 30_000)
          : repository.listMeshNodes('mesh');
    await assert.rejects(pending, (error) => {
      assert.equal(error.kind, ZLinkFrameworkErrorKind.Unavailable);
      assert.equal(error.cause, cause);
      assert.ok(!error.message.includes('private'));
      return true;
    });
  });
}

for (const cause of [
  new TypeError('validation'),
  new RangeError('bounds'),
  new ZLinkFrameworkException(ZLinkFrameworkErrorKind.ProtocolError, 'typed')
]) {
  test(`repository preserves ${cause.name} from the provider`, async () => {
    const repository = new ZLinkLocationStoreRepository({
      read() {
        throw cause;
      }
    });
    await assert.rejects(
      repository.readAuthority(encodeAuthorityKey('user_spot', 'spot')),
      (error) => error === cause
    );
  });
}

test('repository preserves caller cancellation', async () => {
  const controller = new AbortController();
  const cause = new Error('caller cancellation');
  const repository = new ZLinkLocationStoreRepository({
    read() {
      controller.abort(cause);
      return Promise.reject(cause);
    }
  });
  await assert.rejects(
    repository.readAuthority(encodeAuthorityKey('user_spot', 'spot'), controller.signal),
    (error) => error === cause
  );
});

test('repository does not classify record decode failure as provider failure', async () => {
  const repository = new ZLinkLocationStoreRepository({
    async read() {
      return {
        kind: 'found',
        value: {
          bytes: Buffer.from('invalid record'),
          version: { value: '1' },
          storeNow: new Date()
        }
      };
    }
  });
  await assert.rejects(
    repository.readAuthority(encodeAuthorityKey('user_spot', 'spot')),
    (error) => !(error instanceof ZLinkFrameworkException)
  );
});
