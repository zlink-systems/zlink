const assert = require('node:assert/strict');
const test = require('node:test');
const internal = require('../../packages/framework/dist/internal');
const {
  storeKey
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  opaqueRecordPreimage
} = require('../../packages/framework/dist/runtime/locations/opaque-record-key');

const source = {
  nodeRid: 'source-node',
  nodeGeneration: '9007199254740993',
  ownerId: 'source-owner',
  ownerLeaseGeneration: '1'
};
const canonical = {
  relocationId: '00000000-0000-0001-0000-000000000002',
  phase: 'captured',
  source,
  target: null,
  coordinator: { ...source, expectedStoreVersion: 'opaque:baseline/v1' },
  sourceInventorySha256: 'ab'.repeat(32),
  applicationVersion: '9007199254740995'
};

async function fixture(relocation) {
  const provider = new internal.ZLinkInMemoryProviderLocationStore(() => new Date(100));
  const store = new internal.ZLinkLocationStoreRepository(provider, () => new Date(100));
  await store.claimOwnerLease('source-owner', 60000);
  const key = storeKey(opaqueRecordPreimage('authority', 'actor', 'relocation-record'));
  const envelope = {
    recordVersion: 1,
    payload: Buffer.from('opaque application state').toString('base64'),
    objectGeneration: '1',
    authorityOwnerGeneration: '1',
    ownerId: 'source-owner',
    ownerLeaseGeneration: '1',
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'player',
      descriptor: { meshName: 'mesh', routingIdHex: Buffer.from('source-node').toString('hex') },
      descriptorLifecycleGeneration: '9007199254740993',
      capacity: { actors: 1, spots: 0, spotType: null }
    },
    pendingCreation: null,
    ...(relocation === undefined ? {} : { aggregate: { relocation } })
  };
  await provider.write({
    conditions: [{ kind: 'missing', key }],
    mutations: [{ kind: 'put', key, bytes: Buffer.from(JSON.stringify(envelope)) }]
  });
  return {
    provider,
    store,
    key,
    envelope,
    authorityKey: encodeAuthorityKey('actor', 'relocation-record')
  };
}

test('repository reads a cross-language relocation-only authority without treating it as a marker', async () => {
  const { store, authorityKey } = await fixture(canonical);
  const current = await store.readAuthority(authorityKey);
  assert.equal(current.relocation.phase, 'captured');
  assert.equal(current.relocation.source.nodeGeneration, 9007199254740993n);
  assert.equal(current.relocation.coordinator.expectedStoreVersion, 'opaque:baseline/v1');
  assert.equal(Buffer.from(current.payload).toString(), 'opaque application state');
});

test('phase CAS uses aggregate.relocation once, preserves opaque payload and removes the empty aggregate', async () => {
  const { store, provider, authorityKey, key, envelope } = await fixture();
  let current = await store.readAuthority(authorityKey);
  for (const phase of [
    'preparing',
    'captured',
    'prepared',
    'committed',
    'activating',
    'activated',
    'cleaning',
    'completed',
    'aborted'
  ]) {
    const fence = { ...source, nodeGeneration: 9007199254740993n, ownerLeaseGeneration: 1n };
    const relocation = {
      ...canonical,
      phase,
      source: fence,
      coordinator: { ...fence, expectedStoreVersion: 'opaque:baseline/v1' },
      target: ['preparing', 'captured'].includes(phase)
        ? null
        : { ...fence, attemptGeneration: 9007199254740997n },
      applicationVersion: 9007199254740995n
    };
    const result = await store.compareExchangeAuthority(authorityKey, current.storeVersion, {
      kind: 'put',
      generationTransition: 'preserve',
      payload: current.payload,
      relocation
    });
    assert.equal(result.kind, 'stored');
    current = await store.readAuthority(authorityKey);
    const raw = JSON.parse(Buffer.from((await provider.read(key)).value.bytes));
    assert.equal(raw.aggregate.relocation.phase, phase);
    assert.equal(
      raw.aggregate.relocation.target === null,
      ['preparing', 'captured'].includes(phase)
    );
    assert.equal(raw.aggregate.relocation.coordinator.expectedStoreVersion, 'opaque:baseline/v1');
    assert.equal(raw.payload, envelope.payload);
    assert.equal(raw.visibleStoreVersion, undefined);
    assert.deepEqual(current.relocation, relocation);
  }
  const cleaned = await store.compareExchangeAuthority(authorityKey, current.storeVersion, {
    kind: 'put',
    generationTransition: 'preserve',
    payload: current.payload,
    relocation: null
  });
  assert.equal(cleaned.kind, 'stored');
  assert.equal(
    JSON.parse(Buffer.from((await provider.read(key)).value.bytes)).aggregate,
    undefined
  );
});

test('legacy authority without aggregate remains readable without backfilling relocation', async () => {
  const { store, provider, key, authorityKey } = await fixture();
  const before = (await provider.read(key)).value.bytes;
  const current = await store.readAuthority(authorityKey);
  assert.equal(current.kind, 'snapshot');
  assert.equal(current.relocation, undefined);
  assert.deepEqual((await provider.read(key)).value.bytes, before);
});

test('fixed previous Node ZLAR and canonical ZLAU slot records are compatibility reader inputs', async () => {
  const {
    ServiceRelocationAuthorityPayloadCodec,
    serviceRelocationAuthorityApplicationPayload
  } = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');
  const codec = new ServiceRelocationAuthorityPayloadCodec();
  for (const vector of require('./fixtures/legacy-relocation-authority.json')) {
    const payload = Buffer.from(vector.payload, 'base64');
    assert.equal(codec.read(payload).aggregateId, '11111111-1111-4111-8111-111111111111');
    assert.equal(codec.read(payload).aggregateGeneration, 2n);
    assert.deepEqual(
      serviceRelocationAuthorityApplicationPayload(payload),
      Buffer.from(vector.application, 'base64')
    );
    const { store, provider, key, envelope, authorityKey } = await fixture();
    const current = await provider.read(key);
    await provider.write({
      conditions: [{ kind: 'version', key, expected: current.value.version }],
      mutations: [
        {
          kind: 'put',
          key,
          bytes: Buffer.from(JSON.stringify({ ...envelope, payload: vector.payload }))
        }
      ]
    });
    const read = await store.readAuthority(authorityKey);
    assert.deepEqual(Buffer.from(read.payload), payload);
    assert.equal(read.relocation, undefined);
    assert.equal(
      JSON.parse(Buffer.from((await provider.read(key)).value.bytes)).aggregate,
      undefined
    );
  }
});

const aggregateGolden = require('../../../../runtime/protocol/golden/authority-aggregate-relocation-v1.json');
const {
  decodeAuthorityAggregate
} = require('../../packages/framework/dist/runtime/locations/authority-relocation-record');
for (const vector of aggregateGolden.cases.filter(
  (item) => item.expect === 'accept' && item.aggregate.aggregateId === undefined
)) {
  test(`provider writes and rewrites common golden ${vector.name}`, async () => {
    const { store, provider, authorityKey, key, envelope } = await fixture();
    let current = await store.readAuthority(authorityKey);
    const state = decodeAuthorityAggregate(vector.aggregate).relocation;
    const written = await store.compareExchangeAuthority(authorityKey, current.storeVersion, {
      kind: 'put',
      generationTransition: 'preserve',
      payload: current.payload,
      relocation: state
    });
    assert.equal(written.kind, 'stored');
    let raw = JSON.parse(Buffer.from((await provider.read(key)).value.bytes));
    assert.deepEqual(raw.aggregate, vector.aggregate);
    current = await store.readAuthority(authorityKey);
    assert.deepEqual(current.relocation, state);
    const rewritten = await store.compareExchangeAuthority(authorityKey, current.storeVersion, {
      kind: 'put',
      generationTransition: 'preserve',
      payload: current.payload
    });
    assert.equal(rewritten.kind, 'stored');
    raw = JSON.parse(Buffer.from((await provider.read(key)).value.bytes));
    assert.deepEqual(raw.aggregate, vector.aggregate);
    assert.equal(raw.payload, envelope.payload);
  });
}
