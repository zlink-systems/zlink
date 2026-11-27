const {
  ZLinkInMemoryProviderLocationStore
} = require('../../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  RelocationReplyStore
} = require('../../../packages/framework/dist/runtime/locations/relocation-reply-store');

const relocationId = '00000000-0000-0000-0000-000000000001';
const defaultIdentity = {
  relocationId,
  objectKey: 'zla1:a:5:actor',
  source: { ownerId: 'owner-A', leaseGeneration: '9', nodeRid: '41', nodeGeneration: '7' },
  operationId: '00000000000000070000000000000001',
  replyRouteId: '1'
};

function replyFixture(writer = { ownerId: 'target', leaseGeneration: 1n }) {
  let now = 0;
  const provider = new ZLinkInMemoryProviderLocationStore(() => new Date(now));
  const lease = (owner) => ({
    kind: 'value',
    key: { value: `test-lease:${owner.ownerId}` },
    expected: Buffer.from(owner.leaseGeneration.toString())
  });
  const store = new RelocationReplyStore(provider, lease);
  const setWriter = (owner) =>
    provider.write({
      conditions: [],
      mutations: [{ kind: 'put', key: lease(owner).key, bytes: lease(owner).expected }]
    });
  const seed = (identity = defaultIdentity) => {
    const mutation = RelocationReplyStore.accepted(identity, writer);
    return provider.write({
      conditions: [{ kind: 'missing', key: mutation.key }],
      mutations: [mutation]
    });
  };
  const rows = async (id = relocationId) => {
    const found = [];
    for await (const row of store.scan(id)) found.push(row);
    return found;
  };
  return {
    provider,
    store,
    writer,
    setWriter,
    seed,
    rows,
    advance: (ms) => {
      now += ms;
    }
  };
}

module.exports = { replyFixture, defaultIdentity };
