const assert = require('node:assert/strict');
const {
  encodeAuthorityAggregate
} = require('../../../packages/framework/dist/runtime/locations/authority-relocation-record');

const { ZLinkInMemoryLocationStore } = require('../../../packages/framework/dist/internal');

function relocationInventoryStore() {
  const store = new ZLinkInMemoryLocationStore();
  return {
    storeAggregateInventory: store.storeAggregateInventory.bind(store),
    readAggregateInventory: store.readAggregateInventory.bind(store)
  };
}

// A domain-store fixture retains the same authority snapshot across progress CAS operations.
function relocationAuthorityStore(readInitial) {
  const rows = new Map();
  const store = {
    ...relocationInventoryStore(),
    async readAuthority(key) {
      if (!rows.has(key.value)) rows.set(key.value, await readInitial(key));
      return rows.get(key.value);
    },
    async compareExchangeAuthority(key, expected, mutation) {
      const current = await this.readAuthority(key);
      if (current.kind !== 'snapshot' || current.storeVersion.value !== expected.value)
        return { kind: 'conflict', current };
      assert.equal(mutation.kind, 'put');
      assert.equal(mutation.generationTransition, 'preserve');
      if (mutation.relocation != null) encodeAuthorityAggregate(undefined, mutation.relocation);
      const next = {
        ...current,
        payload: Buffer.from(mutation.payload),
        relocation:
          mutation.relocation === undefined
            ? current.relocation
            : (mutation.relocation ?? undefined),
        storeVersion: { value: `${current.storeVersion.value}:cas` }
      };
      rows.set(key.value, next);
      return { ...next, kind: 'stored' };
    }
  };
  return () => store;
}
module.exports = { relocationAuthorityStore, relocationInventoryStore };
