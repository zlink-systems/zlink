const assert = require('node:assert/strict');
const {
  RelocationCompletionStore
} = require('../../../packages/framework/dist/runtime/locations/relocation-completion-store');
const golden = require('../../../../../runtime/protocol/golden/relocation-completion-record-v1.json');
const example = JSON.parse(Buffer.from(golden.cases[0].bytesBase64, 'base64'));
const entry = example.entries[0];
function completionFixture(accepted = 1n, key = { value: entry.objectKey }) {
  const blobs = new Map();
  const expiries = new Map();
  const events = [];
  const fence = {
    ownerId: 'target',
    ownerLeaseGeneration: 1n,
    nodeRid: '746172676574',
    nodeGeneration: 1n
  };
  let authority = {
    kind: 'snapshot',
    storeVersion: { value: '1' },
    payload: Buffer.from('application'),
    allocation: { objectKind: 'actor' },
    objectGeneration: 3n,
    authorityOwnerGeneration: 4n,
    ownerId: 'target',
    ownerLeaseGeneration: 1n,
    relocation: {
      relocationId: example.relocationId,
      phase: 'activated',
      source: fence,
      target: { ...fence, attemptGeneration: 1n },
      coordinator: { ...fence, expectedStoreVersion: 'source-version' },
      sourceInventorySha256: 'ab'.repeat(32),
      applicationVersion: 1n,
      completion: {
        acceptedRequests: accepted,
        completedResults: 0n,
        pendingDeliveries: 0n,
        reference: null,
        sha256: null
      }
    }
  };
  let clock = 100;
  const store = {
    async put(reference, bytes, retention) {
      events.push('put');
      blobs.set(reference.value, Buffer.from(bytes));
      expiries.set(reference.value, clock + retention);
      return { kind: 'stored', storeNow: new Date(clock), expiresAt: new Date(clock + retention) };
    },
    async read(reference) {
      events.push('read');
      return blobs.has(reference.value) && expiries.get(reference.value) > clock
        ? {
            kind: 'found',
            bytes: blobs.get(reference.value),
            storeNow: new Date(clock),
            expiresAt: new Date(expiries.get(reference.value))
          }
        : { kind: 'missing', storeNow: new Date(clock) };
    },
    async renew(reference, retention) {
      events.push(`renew:${reference.value}`);
      if (!blobs.has(reference.value) || expiries.get(reference.value) <= clock)
        return { kind: 'missing', storeNow: new Date(clock) };
      expiries.set(reference.value, clock + retention);
      return { kind: 'renewed', storeNow: new Date(clock), expiresAt: new Date(clock + retention) };
    },
    async delete(reference) {
      events.push(`delete:${reference.value}`);
      blobs.delete(reference.value);
      expiries.delete(reference.value);
    }
  };
  const authorities = {
    async compareExchangeAuthority(actual, version, mutation) {
      events.push('cas');
      assert.deepEqual(actual, key);
      assert.equal(version.value, authority.storeVersion.value);
      authority = {
        ...authority,
        ...mutation,
        kind: 'snapshot',
        storeVersion: { value: String(Number(version.value) + 1) }
      };
      assert.equal(authority.payload.toString(), 'application');
      assert.equal(authority.objectGeneration, 3n);
      assert.equal(authority.authorityOwnerGeneration, 4n);
      return { ...authority, kind: 'stored' };
    }
  };
  return {
    key,
    store,
    authorities,
    blobs,
    events,
    owner: new RelocationCompletionStore(authorities, store),
    current: () => authority,
    advance: (ms = 86399900) => {
      clock += ms;
    }
  };
}

module.exports = { completionFixture, example, entry };
