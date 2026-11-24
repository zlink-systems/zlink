const assert = require('node:assert/strict');
const test = require('node:test');
const {
  encodeAuthorityAggregate,
  decodeAuthorityAggregate
} = require('../../packages/framework/dist/runtime/locations/authority-relocation-record.js');

const fence = {
  nodeRid: '6e6f64652d61',
  nodeGeneration: 9007199254740993n,
  ownerId: 'owner-a',
  ownerLeaseGeneration: 9007199254740995n
};
const relocation = {
  relocationId: '00000000-0000-0001-0000-000000000002',
  phase: 'preparing',
  source: fence,
  target: null,
  coordinator: { ...fence, expectedStoreVersion: 'opaque:original/1' },
  sourceInventorySha256: 'ab'.repeat(32),
  applicationVersion: 9007199254740997n
};
const marker = {
  aggregateId: relocation.relocationId,
  aggregateGeneration: 7n,
  index: 0,
  expectedStoreVersion: 'opaque:marker/2',
  ownerTransition: 'newOwner',
  targetAuthorityOwnerGeneration: 11n,
  authorityPayloadSha256: '01'.repeat(32),
  membershipMutationSha256: '02'.repeat(32)
};

test('authority aggregate stores all relocation phases with decimal strings and exact baseline', () => {
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
    const state = {
      ...relocation,
      phase,
      target:
        phase === 'preparing' || phase === 'captured'
          ? null
          : { ...fence, attemptGeneration: 9007199254740999n }
    };
    const encoded = encodeAuthorityAggregate(undefined, state);
    assert.deepEqual(encoded, {
      relocation: {
        ...state,
        source: {
          ...fence,
          nodeGeneration: '9007199254740993',
          ownerLeaseGeneration: '9007199254740995'
        },
        target:
          state.target === null
            ? null
            : {
                ...fence,
                nodeGeneration: '9007199254740993',
                ownerLeaseGeneration: '9007199254740995',
                attemptGeneration: '9007199254740999'
              },
        coordinator: {
          ...fence,
          nodeGeneration: '9007199254740993',
          ownerLeaseGeneration: '9007199254740995',
          expectedStoreVersion: 'opaque:original/1'
        },
        applicationVersion: '9007199254740997'
      }
    });
    assert.deepEqual(
      decodeAuthorityAggregate(JSON.parse(JSON.stringify(encoded))).relocation,
      state
    );
  }
});

test('marker materialization and removal preserve relocation, terminal cleanup omits aggregate', () => {
  const encoded = encodeAuthorityAggregate(marker, relocation);
  assert.deepEqual(encoded.relocation, encodeAuthorityAggregate(undefined, relocation).relocation);
  assert.deepEqual(decodeAuthorityAggregate(encoded), { marker, relocation });
  assert.deepEqual(decodeAuthorityAggregate(encodeAuthorityAggregate(undefined, relocation)), {
    marker: undefined,
    relocation
  });
  assert.equal(encodeAuthorityAggregate(undefined, undefined), undefined);
  assert.deepEqual(decodeAuthorityAggregate(encodeAuthorityAggregate(marker, undefined)), {
    marker,
    relocation: undefined
  });
  assert.deepEqual(decodeAuthorityAggregate(undefined), {
    marker: undefined,
    relocation: undefined
  });
});

test('authority aggregate rejects empty, partial marker, partial target and wrong phase target', () => {
  for (const invalid of [
    {},
    { aggregateId: marker.aggregateId },
    {
      relocation: {
        ...encodeAuthorityAggregate(undefined, relocation).relocation,
        phase: 'prepared'
      }
    },
    { relocation: { ...encodeAuthorityAggregate(undefined, relocation).relocation, target: {} } }
  ]) {
    assert.throws(() => decodeAuthorityAggregate(invalid));
  }
});

for (const field of ['source', 'target', 'coordinator']) {
  const invalidValues = [
    ['empty RID', { nodeRid: '' }],
    ['nonhex RID', { nodeRid: 'aazz' }],
    ['odd length RID', { nodeRid: 'abc' }],
    ['uppercase RID', { nodeRid: 'AB' }],
    ['oversized RID', { nodeRid: 'aa'.repeat(256) }],
    ['zero node generation', { nodeGeneration: '0' }],
    ['zero lease generation', { ownerLeaseGeneration: '0' }],
    ...(field === 'target' ? [['zero attempt generation', { attemptGeneration: '0' }]] : [])
  ];
  for (const [name, invalid] of invalidValues) {
    test(`authority relocation rejects ${field} fence with ${name}`, () => {
      const encoded = encodeAuthorityAggregate(undefined, {
        ...relocation,
        phase: 'prepared',
        target: { ...fence, attemptGeneration: 1n }
      });
      const malformed = {
        relocation: {
          ...encoded.relocation,
          [field]: { ...encoded.relocation[field], ...invalid }
        }
      };
      assert.throws(() => decodeAuthorityAggregate(malformed), TypeError);
    });
  }
}

test('authority relocation accepts the maximum RID length and full uint64 fence values', () => {
  const maxFence = {
    ...fence,
    nodeRid: 'ab'.repeat(255),
    nodeGeneration: 0xffffffffffffffffn,
    ownerLeaseGeneration: 0xffffffffffffffffn
  };
  const prepared = {
    ...relocation,
    phase: 'prepared',
    source: maxFence,
    target: { ...maxFence, attemptGeneration: 0xffffffffffffffffn },
    coordinator: { ...relocation.coordinator, ...maxFence },
    applicationVersion: 0n
  };
  assert.deepEqual(
    decodeAuthorityAggregate(encodeAuthorityAggregate(undefined, prepared)).relocation,
    prepared
  );
});

const golden = require('../../../../runtime/protocol/golden/authority-aggregate-relocation-v1.json');
for (const vector of golden.cases) {
  test(`common aggregate golden: ${vector.name} (${vector.expect})`, () => {
    if (vector.expect === 'reject') {
      assert.throws(() => decodeAuthorityAggregate(vector.aggregate));
      return;
    }
    const decoded = decodeAuthorityAggregate(vector.aggregate);
    const written = encodeAuthorityAggregate(decoded.marker, decoded.relocation);
    assert.deepEqual(written, vector.aggregate);
    const reread = decodeAuthorityAggregate(JSON.parse(JSON.stringify(written)));
    assert.deepEqual(encodeAuthorityAggregate(reread.marker, reread.relocation), vector.aggregate);
  });
}
