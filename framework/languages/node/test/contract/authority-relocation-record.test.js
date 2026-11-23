const assert = require('node:assert/strict');
const test = require('node:test');
const {
  encodeAuthorityAggregate,
  decodeAuthorityAggregate
} = require('../../packages/framework/dist/runtime/locations/authority-relocation-record.js');

const fence = {
  nodeRid: 'node-a',
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
