const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { createMeshHost } = require('./fixtures/mesh-host');

const root = (id) => 'zlink:v11:aggregate:' + id + ':1';
const capacity = 'zlink:v11:capacity:mesh:target';
const child = 'zlink:v11:aggregate-inventory:committed:1:root';
const lease = 'owner-lease\0target-owner';
const marker = 'zlink:v12:format';
const pending = ['zlink:v12:capacity', 'mesh', '746172676574', 'actors', 'pending'].join('\0');

function aggregate(state, actors = 0, ownerId = 'target-owner') {
  return {
    state,
    stagedAt: Date.now(),
    targetOwner: { ownerId, leaseGeneration: { $bigint: '1' } },
    targetDescriptor: { meshName: 'mesh', rid: 'target' },
    targetDescriptorLifecycleGeneration: { $bigint: '1' },
    capacity: { actors, spots: 0 }
  };
}

function actorAuthority(payload) {
  return {
    recordVersion: 1,
    payload: Buffer.from(payload).toString('base64'),
    objectGeneration: '3',
    authorityOwnerGeneration: '4',
    ownerId: 'source',
    ownerLeaseGeneration: '1',
    allocation: {
      state: 'active',
      objectKind: 'actor',
      stableType: 'Player',
      descriptor: { meshName: 'mesh', routingIdHex: '736f75726365' },
      descriptorLifecycleGeneration: '1',
      capacity: { actors: 1, spots: 0, spotType: null }
    },
    pendingCreation: null
  };
}

async function seed(store, rows) {
  const result = await store.write({
    conditions: [],
    mutations: rows.map(([value, record, retentionMs]) => ({
      kind: 'put',
      key: { value },
      bytes: Buffer.isBuffer(record) ? record : Buffer.from(JSON.stringify(record)),
      ...(retentionMs ? { retentionMs } : {})
    }))
  });
  assert.equal(result.kind, 'applied');
}

async function snapshot(store) {
  const page = await store.scan({ prefix: '', limit: 1000 });
  return Object.fromEntries(
    page.value.items.map(({ key, value }) => [key.value, Buffer.from(value.bytes)])
  );
}

function startup(t, store, rid = 'conversion') {
  const { host } = createMeshHost({
    registration: framework.createFrameworkRegistration({ locations: { storeInstance: store } })
  });
  const runtime = host.locationOwner.ensureRuntime('mesh');
  t.after(() => runtime.stop());
  return runtime.start(rid);
}

test('startup settles fresh v11 staging and prepared roots and cleans committed children', async (t) => {
  let storeNow = Date.now();
  const store = new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow));
  await seed(store, [
    [lease, { recordVersion: 1, ownerId: 'target-owner', leaseGeneration: '1' }, 1],
    [root('fresh'), aggregate('staging')],
    [root('prepared'), aggregate('prepared', 2)],
    [root('committed'), aggregate('committed')],
    [child, { inventory: true }],
    [
      capacity,
      {
        active: { actors: 1, spots: 0, spotTypes: {} },
        pending: { actors: 3, spots: 0, spotTypes: {} }
      }
    ]
  ]);
  storeNow += 2;
  const starting = await Promise.all([startup(t, store), startup(t, store, 'conversion-peer')]);
  assert.deepEqual(starting, [undefined, undefined]);
  const rows = await snapshot(store);
  assert.equal(rows[marker]?.toString(), 'converted');
  assert.equal(rows[pending]?.toString(), '1');
  for (const key of [root('fresh'), root('prepared'), root('committed'), child, capacity]) {
    assert.equal(rows[key], undefined, key);
  }
});

test('startup leaves the entire Store unchanged when any v11 aggregate target lease is live', async (t) => {
  let storeNow = Date.now();
  const store = new framework.ZLinkInMemoryProviderLocationStore(() => new Date(storeNow));
  await seed(store, [
    [root('first'), aggregate('committed', 0, 'expired-owner')],
    [root('live'), aggregate('committed')],
    [lease, { recordVersion: 1, ownerId: 'target-owner', leaseGeneration: '1' }, 60_000]
  ]);
  const before = await snapshot(store);
  await assert.rejects(startup(t, store), {
    name: 'ZLinkConfigurationException',
    message: /live.*lease/i
  });
  assert.deepEqual(await snapshot(store), before);
  storeNow += 60_001;
  await startup(t, store, 'conversion-after-expiry');
  const rows = await snapshot(store);
  assert.equal(rows[marker]?.toString(), 'converted');
  assert.equal(rows[root('first')], undefined);
  assert.equal(rows[root('live')], undefined);
});

test('startup releases Java and C++ prepared capacity and removes .NET terminal children', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const uuid = '01020304-0506-0708-090a-0b0c0d0e0f10';
  const compactId = uuid.replaceAll('-', '');
  const cppId = '0123456789abcdef0123456789abcdef';
  const javaRoot = root(uuid);
  const cppRoot = 'zlink:v11:aggregate:' + cppId;
  const dotnetRoot = root(compactId);
  const javaChild = 'zlink:v11:aggregate-inventory:' + uuid + ':1:participant:0:payload';
  const cppChild = 'zlink:v11:aggregate-inventory:' + cppId + ':0';
  const cppLock = 'zlink:v11:aggregate-lock:' + 'a'.repeat(64);
  const dotnetParticipant = dotnetRoot + ':participant:0:meta';
  const dotnetInventory = dotnetRoot + ':inventory:0:0';
  const java = Buffer.from(
    [
      '01',
      compactId,
      '0000000000000001',
      '00046d657368',
      '000c373436313732363736353734',
      '0000000000000001',
      '00016f',
      '0000000000000001',
      '00000020',
      '00'.repeat(32),
      '00000001',
      '00000000',
      '00',
      '00000001',
      '00000020',
      '00'.repeat(32)
    ].join(''),
    'hex'
  );
  const targetOwner = { ownerId: 'o', leaseGeneration: '1' };
  await seed(store, [
    [javaRoot, java],
    [javaChild, {}],
    [
      cppRoot,
      {
        status: 'prepared',
        aggregateId: cppId,
        aggregateGeneration: '18446744073709551615',
        targetMeshName: 'mesh',
        targetNodeRid: 'cpp',
        targetLifecycleGeneration: 1,
        targetOwner,
        capacityBundle: {
          actors: 0,
          spots: 1,
          spotType: { objectKind: 'userSpot', stableType: 'Table', count: 1 }
        }
      }
    ],
    [cppChild, {}],
    [cppLock, { aggregateId: cppId, aggregateGeneration: '18446744073709551615' }],
    [
      'zlink:v11:capacity:mesh:cpp',
      {
        actorsActive: 0,
        actorsPending: 0,
        spotsActive: 0,
        spotsPending: 1,
        spotTypes: { '2:Table': { active: 0, pending: 1 } }
      }
    ],
    [
      dotnetRoot,
      {
        status: 2,
        participantsNormalized: true,
        header: {
          aggregateId: uuid,
          aggregateGeneration: 1,
          targetDescriptor: { meshName: 'mesh', routingIdHex: '746172676574' },
          targetDescriptorLifecycleGeneration: 1,
          targetOwner,
          capacity: { actors: 0, spots: 0, spotType: null }
        }
      }
    ],
    [dotnetParticipant, {}],
    [dotnetInventory, {}],
    [
      capacity,
      {
        active: { actors: 0, spots: 0, spotTypes: {} },
        pending: { actors: 2, spots: 0, spotTypes: {} }
      }
    ]
  ]);
  await startup(t, store);
  const rows = await snapshot(store);
  assert.equal(rows[marker]?.toString(), 'converted');
  assert.equal(rows[pending]?.toString(), '1');
  for (const key of [
    javaRoot,
    cppRoot,
    dotnetRoot,
    javaChild,
    cppChild,
    cppLock,
    dotnetParticipant,
    dotnetInventory
  ]) {
    assert.equal(rows[key], undefined, key);
  }
  assert.equal(
    rows[['zlink:v12:capacity', 'mesh', '637070', 'spots', 'pending'].join('\0')],
    undefined
  );
  assert.equal(
    rows[
      ['zlink:v12:capacity', 'mesh', '637070', 'spot-type', 'userSpot', 'Table', 'pending'].join(
        '\0'
      )
    ],
    undefined
  );
});

test('startup rolls back a partially published C++ aggregate before deleting its commit page', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const id = '00112233445566778899aabbccddeeff';
  const cppRoot = 'zlink:v11:aggregate:' + id;
  const page = 'zlink:v11:aggregate-commit:' + id + ':0';
  const logical = encodeAuthorityKey('actor', 'rollback').value;
  const authority = 'authority\0actor\0rollback';
  const lock = 'zlink:v11:aggregate-lock:' + createHash('sha256').update(logical).digest('hex');
  const before = Buffer.from(JSON.stringify(actorAuthority('source authority')));
  const after = Buffer.from(JSON.stringify(actorAuthority('partly published target authority')));
  await seed(store, [
    [
      cppRoot,
      {
        status: 'committing',
        aggregateId: id,
        aggregateGeneration: 1,
        commitPageCount: 1,
        targetMeshName: 'mesh',
        targetNodeRid: 'target',
        targetLifecycleGeneration: 1,
        targetOwner: { ownerId: 'expired', leaseGeneration: 1 },
        capacityBundle: { actors: 1, spots: 0, spotType: null }
      }
    ],
    [
      page,
      {
        version: 1,
        pageIndex: 0,
        entries: [
          { authorityKey: logical, before: before.toString('hex'), after: after.toString('hex') }
        ]
      }
    ],
    [
      lock,
      {
        aggregateId: id,
        aggregateGeneration: 1,
        authorityKey: logical,
        status: 'committing',
        pageIndex: 0,
        entryIndex: 0
      }
    ],
    [authority, after],
    [
      capacity,
      {
        actorsActive: 0,
        actorsPending: 2,
        spotsActive: 0,
        spotsPending: 0,
        spotTypes: {}
      }
    ]
  ]);
  await startup(t, store);
  const rows = await snapshot(store);
  assert.equal(rows[marker]?.toString(), 'converted');
  assert.deepEqual(rows[authority], before);
  assert.equal(rows[pending]?.toString(), '1');
  for (const key of [cppRoot, page, lock]) assert.equal(rows[key], undefined, key);
});

test('startup publishes a previously committed .NET participant before deleting its payload', async (t) => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  const uuid = '11020304-0506-0708-090a-0b0c0d0e0f10';
  const aggregateGeneration = '9007199254740993';
  const dotnetRoot = 'zlink:v11:aggregate:' + uuid.replaceAll('-', '') + ':' + aggregateGeneration;
  const authority = 'authority\0actor\0dotnet';
  const meta = dotnetRoot + ':participant:0:meta';
  const payload = dotnetRoot + ':participant:0:payload';
  const committed = Buffer.from('committed target payload');
  await seed(store, [
    [
      dotnetRoot,
      {
        status: 2,
        participantsNormalized: false,
        header: {
          aggregateId: uuid,
          aggregateGeneration,
          targetOwner: { ownerId: 'target-owner', leaseGeneration: 2 },
          targetDescriptor: { meshName: 'mesh', routingIdHex: '746172676574' },
          targetDescriptorLifecycleGeneration: 3,
          capacity: { actors: 1, spots: 0 }
        }
      }
    ],
    [
      meta,
      {
        index: 0,
        key: encodeAuthorityKey('actor', 'dotnet').value,
        ownerTransition: 2,
        authorityPayloadSha256: createHash('sha256').update(committed).digest('base64'),
        targetAuthorityOwnerGeneration: '9007199254740995'
      }
    ],
    [payload, committed],
    [
      authority,
      {
        ...actorAuthority('source payload'),
        aggregateFence: { aggregateId: uuid, aggregateGeneration },
        aggregateParticipantIndex: 0
      }
    ]
  ]);
  await startup(t, store);
  const rows = await snapshot(store);
  const stored = JSON.parse(rows[authority].toString());
  assert.equal(rows[marker]?.toString(), 'converted');
  assert.deepEqual(Buffer.from(stored.payload, 'base64'), committed);
  assert.equal(stored.ownerId, 'target-owner');
  assert.equal(stored.authorityOwnerGeneration, '9007199254740995');
  assert.equal(stored.allocation.descriptor.routingIdHex, '746172676574');
  assert.equal(stored.aggregateFence, undefined);
  for (const key of [dotnetRoot, meta, payload]) assert.equal(rows[key], undefined, key);
});
