const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');

function descriptor(owner, overrides = {}) {
  return {
    channelName: 'events',
    publisherRid: 'publisher-a',
    lifecycleGeneration: 7n,
    descriptorRevision: 1n,
    endpoint: 'tcp://10.0.0.1:9501',
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'cluster-a',
    ownerId: owner.token.ownerId,
    leaseGeneration: owner.token.leaseGeneration,
    updatedAt: new Date(0),
    ...overrides
  };
}

test('in-memory fanout descriptor store fences lifecycle, revision, and immutable identity', async () => {
  let now = new Date(Date.UTC(2026, 6, 24, 0, 0, 0));
  const store = new internal.ZLinkInMemoryLocationStore(() => now);
  const owner = await store.claimOwnerLease('publisher-owner', 30_000);
  assert.equal(owner.kind, 'claimed');

  const claimed = await store.updateFanoutPublisher(
    descriptor(owner),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  assert.equal(claimed.status, internal.ZLinkLocationWriteStatus.Stored);

  const mutableWithoutRevision = await store.updateFanoutPublisher(
    descriptor(owner, { state: framework.ZLinkFrameworkRuntimeState.Draining }),
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(mutableWithoutRevision.status, internal.ZLinkLocationWriteStatus.IgnoredStale);

  const immutableChange = await store.updateFanoutPublisher(
    descriptor(owner, {
      descriptorRevision: 2n,
      endpoint: 'tcp://10.0.0.2:9501'
    }),
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(immutableChange.status, internal.ZLinkLocationWriteStatus.IgnoredStale);

  const lifecycleChange = await store.updateFanoutPublisher(
    descriptor(owner, {
      lifecycleGeneration: 8n,
      descriptorRevision: 2n
    }),
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(lifecycleChange.status, internal.ZLinkLocationWriteStatus.IgnoredStale);

  const renewed = await store.updateFanoutPublisher(
    descriptor(owner, {
      descriptorRevision: 2n,
      state: framework.ZLinkFrameworkRuntimeState.Draining
    }),
    internal.ZLinkLocationWriteIntent.Renew
  );
  assert.equal(renewed.status, internal.ZLinkLocationWriteStatus.Stored);

  await store.updateFanoutPublisher(
    descriptor(owner, {
      channelName: 'other-events',
      publisherRid: 'publisher-b'
    }),
    internal.ZLinkLocationWriteIntent.NewClaim
  );
  const page = await store.listFanoutPublishers('events', { pageSize: 1 });
  assert.equal(page.items.length, 1);
  assert.equal(page.items[0].descriptorRevision, 2n);
  assert.equal(page.items[0].state, framework.ZLinkFrameworkRuntimeState.Draining);

  assert.equal(
    await store.removeFanoutPublisher(
      { channelName: 'events', publisherRid: 'publisher-a' },
      { ownerId: owner.token.ownerId, leaseGeneration: owner.token.leaseGeneration + 1n }
    ),
    internal.ZLinkLocationWriteStatus.IgnoredStale
  );
  assert.equal(
    await store.removeFanoutPublisher(
      { channelName: 'events', publisherRid: 'publisher-a' },
      owner.token
    ),
    internal.ZLinkLocationWriteStatus.Stored
  );
  assert.equal((await store.listFanoutPublishers('events')).items.length, 0);

  assert.equal((await store.listFanoutPublishers('other-events')).items.length, 1);
  now = new Date(now.getTime() + 30_001);
  assert.equal((await store.listFanoutPublishers('other-events')).items.length, 0);
});

test('location runtime owner exposes the dedicated fanout capability from in-memory stores', () => {
  const host = new internal.ZLinkFrameworkRuntimeHost({
    registration: internal.createFrameworkRegistration({
      locations: { useInMemoryStores: true }
    })
  });

  host.createActorManagerOptions();
  const stores = host.locationOwner.currentStores;
  assert.equal(stores.fanoutStore, stores.locationStore);
  assert.equal(typeof stores.fanoutStore.updateFanoutPublisher, 'function');
  assert.equal(typeof stores.fanoutStore.removeFanoutPublisher, 'function');
  assert.equal(typeof stores.fanoutStore.listFanoutPublishers, 'function');
});

test('automatic fanout registration requires only the opaque location provider SPI', () => {
  assert.throws(() => internal.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: [] },
        publishHandlers: [{ packetName: 'Event', handler: { handle() {} } }]
      }
    },
    locations: { storeInstance: {} }
  }), /Location Store must implement read, write, and scan/);

  assert.throws(() => internal.createFrameworkRegistration({
    channels: { events: { routingId: 'publisher', publisher: { bind: 'tcp://127.0.0.1:9501' } } },
    locations: { storeInstance: {} }
  }), /Location Store must implement read, write, and scan/);

  const provider = new internal.ZLinkInMemoryProviderLocationStore();
  assert.doesNotThrow(() => internal.createFrameworkRegistration({
    channels: { events: { routingId: 'publisher', publisher: { bind: 'tcp://127.0.0.1:9501' } } },
    locations: { storeInstance: provider }
  }));
  assert.equal(provider.updateFanoutPublisher, undefined);

  assert.doesNotThrow(() => internal.createFrameworkRegistration({
    channels: {
      events: {
        subscriber: { manualConnections: ['tcp://127.0.0.1:9501'] },
        publishHandlers: [{ packetName: 'Event', handler: { handle() {} } }]
      }
    }
  }));
  assert.doesNotThrow(() => internal.createFrameworkRegistration({
    channels: { events: { publisher: { bind: 'tcp://127.0.0.1:9501' } } }
  }));
});
