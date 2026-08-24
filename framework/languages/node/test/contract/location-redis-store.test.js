const assert = require('node:assert/strict');
const fs = require('node:fs');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');
const { createClient } = require('redis');
const framework = require('../../packages/framework/dist');
const redisLocations = require('../../packages/framework-locations-redis/dist');
const frameworkInternal = require('../../packages/framework/dist/internal');
const {
  decodeAuthorityKey,
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

// Mirrors location-store-repository.ts's authorityKey(): the public opaque
// record preimage is `authority\0{actor|spot}\0{Id}`
// (21-location-runtime.md#2.4) -- Entry/User/Instance spot kinds share the
// "spot" segment, so this test probes the row under the same logical key
// the repository writes to.
function authorityPreimage(authorityKey) {
  const decoded = decodeAuthorityKey(authorityKey);
  const segment = decoded.kind === 'actor' ? 'actor' : 'spot';
  return `authority\0${segment}\0${decoded.globalId}`;
}

test('redis provider exports only the two opaque Store implementations', () => {
  assert.deepEqual(
    Object.keys(redisLocations).sort(),
    ['ZLinkRedisLocationStore', 'ZLinkRedisRelocationStore']
  );
  const locationMethods = Object.getOwnPropertyNames(
    redisLocations.ZLinkRedisLocationStore.prototype
  ).sort();
  for (const method of ['dispose', 'read', 'scan', 'write']) {
    assert.equal(locationMethods.includes(method), true);
  }
  for (const removed of ['claimOwnerLease', 'resolveSpot', 'reserve', 'prepareAggregate']) {
    assert.equal(locationMethods.includes(removed), false);
  }
  const relocationMethods = Object.getOwnPropertyNames(
    redisLocations.ZLinkRedisRelocationStore.prototype
  ).sort();
  for (const method of ['delete', 'dispose', 'put', 'read', 'renew']) {
    assert.equal(relocationMethods.includes(method), true);
  }
  for (const removed of ['putRelocation', 'getRelocation', 'renewRelocation']) {
    assert.equal(relocationMethods.includes(removed), false);
  }
});

test('redis opaque Location Store applies conditional batches atomically', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const prefix = testPrefix('location-batch');
  const store = new redisLocations.ZLinkRedisLocationStore({
    url: fixture.url,
    keyPrefix: prefix
  });
  const alpha = key('object/alpha');
  const beta = key('object/beta');

  try {
    const missing = await store.read(alpha);
    assert.equal(missing.kind, 'missing');
    assert.ok(missing.storeNow instanceof Date);

    const first = await store.write({
      conditions: [{ kind: 'missing', key: alpha }, { kind: 'missing', key: beta }],
      mutations: [
        { kind: 'put', key: alpha, bytes: Uint8Array.from([0, 1, 255]) },
        { kind: 'put', key: beta, bytes: Buffer.from('before') }
      ]
    });
    assert.equal(first.kind, 'applied');
    assert.equal(first.putVersions.length, 2);
    const alphaVersion = first.putVersions.find(item => item.key.value === alpha.value).version;

    const conflict = await store.write({
      conditions: [
        { kind: 'version', key: alpha, expected: version('stale') },
        { kind: 'version', key: beta, expected: first.putVersions[1].version }
      ],
      mutations: [
        { kind: 'put', key: alpha, bytes: Buffer.from('changed') },
        { kind: 'delete', key: beta }
      ]
    });
    assert.equal(conflict.kind, 'conflict');
    assert.deepEqual([...((await store.read(alpha)).value.bytes)], [0, 1, 255]);
    assert.equal(Buffer.from((await store.read(beta)).value.bytes).toString(), 'before');

    const updated = await store.write({
      conditions: [{ kind: 'version', key: alpha, expected: alphaVersion }],
      mutations: [{ kind: 'put', key: alpha, bytes: Buffer.from('after') }]
    });
    assert.equal(updated.kind, 'applied');
    const read = await store.read(alpha);
    assert.equal(read.kind, 'found');
    assert.equal(Buffer.from(read.value.bytes).toString(), 'after');
    assert.notEqual(read.value.version.value, alphaVersion.value);
    assert.equal(read.value.storeNow.getTime() <= Date.now(), true);
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('redis Store serializes concurrent first-use connection', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const prefix = testPrefix('concurrent-first-use');
  const store = new redisLocations.ZLinkRedisLocationStore({
    url: fixture.url,
    keyPrefix: prefix
  });

  try {
    const reads = await Promise.all(
      Array.from({ length: 16 }, (_, index) => store.read(key(`missing/${index}`)))
    );
    assert.equal(reads.length, 16);
    assert.equal(reads.every(result => result.kind === 'missing'), true);
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('redis opaque Location Store enforces TTL and fixed scan snapshots', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const prefix = testPrefix('location-scan');
  const store = new redisLocations.ZLinkRedisLocationStore({
    url: fixture.url,
    keyPrefix: prefix
  });

  try {
    const expiring = key('ttl/item');
    await store.write({
      conditions: [{ kind: 'missing', key: expiring }],
      mutations: [{ kind: 'put', key: expiring, bytes: Buffer.from('ttl'), retentionMs: 20 }]
    });
    await new Promise(resolve => setTimeout(resolve, 40));
    assert.equal((await store.read(expiring)).kind, 'missing');

    await store.write({
      conditions: [],
      mutations: [
        { kind: 'put', key: key('scan/a'), bytes: Buffer.from('A') },
        { kind: 'put', key: key('scan/b'), bytes: Buffer.from('B') }
      ]
    });
    const first = await store.scan({ prefix: 'scan/', limit: 1 });
    assert.equal(first.kind, 'page');
    assert.equal(first.value.items.length, 1);
    assert.ok(first.value.nextCursor);

    await store.write({
      conditions: [],
      mutations: [
        { kind: 'delete', key: key('scan/b') },
        { kind: 'put', key: key('scan/c'), bytes: Buffer.from('C') }
      ]
    });
    const second = await store.scan({
      prefix: 'scan/',
      cursor: first.value.nextCursor,
      limit: 10
    });
    assert.equal(second.kind, 'page');
    assert.deepEqual(
      [...first.value.items, ...second.value.items].map(item => item.key.value),
      ['scan/a', 'scan/b']
    );

    const expired = await store.scan({
      prefix: 'scan/',
      cursor: cursor('00000000-0000-0000-0000-000000000000:'),
      limit: 10
    });
    assert.deepEqual(expired, { kind: 'expired' });
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('redis Location Store persists and validates a 10,100-participant inventory as bounded pages', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const prefix = testPrefix('aggregate-inventory');
  const store = new redisLocations.ZLinkRedisLocationStore({
    url: fixture.url,
    keyPrefix: prefix
  });
  const inventory = new frameworkInternal.ZLinkAggregateInventoryStore(store);
  const aggregateId = '33333333-3333-4333-8333-333333333333';
  const participants = Array.from({ length: 10_100 }, (_, index) => ({
    authorityKey: key(`actor:${index.toString().padStart(5, '0')}`),
    expectedStoreVersion: version(`version-${index}`),
    ownerTransition: 'newOwner',
    authorityPayload: Buffer.from(`authority-${index}`),
    membershipMutation: Buffer.from(`membership-${index}`)
  }));
  const request = {
    aggregateId: { value: aggregateId },
    aggregateGeneration: 1n,
    participants,
    inventoryDigest: Buffer.alloc(32, 7),
    targetDescriptor: { meshName: 'play', rid: 'node-b' },
    targetDescriptorLifecycleGeneration: 2n,
    capacity: {
      actors: 10_099,
      spots: 1,
      spotType: { objectKind: 'user_spot', stableType: 'lobby', count: 1 }
    },
    targetOwner: { ownerId: 'owner-b', leaseGeneration: 2n }
  };

  try {
    await inventory.store(request);
    const restored = await inventory.read(
      {
        aggregateId: request.aggregateId,
        aggregateGeneration: request.aggregateGeneration
      },
      request.inventoryDigest
    );
    assert.equal(restored.length, 10_100);
    assert.equal(restored[0].authorityKey, 'actor:00000');
    assert.equal(restored[10_099].authorityKey, 'actor:10099');

    const logicalPrefix =
      `zlink:v11:aggregate-inventory:${aggregateId}:1:`;
    const values = [];
    let cursor;
    do {
      const page = await store.scan({
        prefix: logicalPrefix,
        ...(cursor === undefined ? {} : { cursor }),
        limit: 1_000
      });
      assert.equal(page.kind, 'page');
      values.push(...page.value.items);
      cursor = page.value.nextCursor;
    } while (cursor !== undefined);

    assert.equal(values.filter(item => item.key.value.endsWith('root')).length, 1);
    const pages = values.filter(item => item.key.value.includes(':page:'));
    assert.ok(pages.length > 1);
    for (const item of pages) {
      assert.ok(item.value.bytes.byteLength <= 1024 * 1024);
      const decoded = JSON.parse(Buffer.from(item.value.bytes).toString('utf8'));
      assert.ok(decoded.entries.length <= 1_024);
      assert.ok(decoded.children.length <= 1_024);
    }

    const damaged = pages[0];
    await store.write({
      conditions: [{
        kind: 'version',
        key: damaged.key,
        expected: damaged.value.version
      }],
      mutations: [{
        kind: 'put',
        key: damaged.key,
        bytes: Buffer.from('damaged')
      }]
    });
    await assert.rejects(
      inventory.read(
        {
          aggregateId: request.aggregateId,
          aggregateGeneration: request.aggregateGeneration
        },
        request.inventoryDigest
      ),
      /checksum|Invalid inventory page/
    );
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('redis-backed aggregate prepare commit and abort converge across repository instances', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const prefix = testPrefix('aggregate-state-machine');
  const providerA = new redisLocations.ZLinkRedisLocationStore({
    url: fixture.url,
    keyPrefix: prefix
  });
  const providerB = new redisLocations.ZLinkRedisLocationStore({
    url: fixture.url,
    keyPrefix: prefix
  });
  const source = new frameworkInternal.ZLinkLocationStoreRepository(providerA);
  const recovery = new frameworkInternal.ZLinkLocationStoreRepository(providerB);

  try {
    const sourceOwner = await source.claimOwnerLease('source-owner', 60_000);
    const targetOwner = await recovery.claimOwnerLease('target-owner', 60_000);
    assert.equal(sourceOwner.kind, 'claimed');
    assert.equal(targetOwner.kind, 'claimed');
    const sourceTarget = {
      meshName: 'play',
      nodeRid: 'source-node',
      nodeLifecycleGeneration: 1n,
      owner: sourceOwner.token
    };
    const targetTarget = {
      meshName: 'play',
      nodeRid: 'target-node',
      nodeLifecycleGeneration: 1n,
      owner: targetOwner.token
    };
    assert.equal(
      (await source.updateMeshNode(
        aggregateDescriptor(sourceTarget, 64),
        frameworkInternal.ZLinkLocationWriteIntent.NewClaim
      )).status,
      frameworkInternal.ZLinkLocationWriteStatus.Stored
    );
    assert.equal(
      (await recovery.updateMeshNode(
        aggregateDescriptor(targetTarget, 64),
        frameworkInternal.ZLinkLocationWriteIntent.NewClaim
      )).status,
      frameworkInternal.ZLinkLocationWriteStatus.Stored
    );

    const snapshots = [];
    for (let index = 0; index < 2; index++) {
      snapshots.push(await createReadyUserSpot(
        source,
        `room-${index.toString().padStart(2, '0')}`,
        sourceTarget
      ));
    }
    const sourceDescriptor = (await source.listMeshNodes('play')).items.find(row =>
      String(row.rid) === String(sourceTarget.nodeRid));
    assert.ok(sourceDescriptor);
    assert.deepEqual(sourceDescriptor.populationCapacity.spots, {
      active: 2,
      reserved: 0,
      limit: 64
    });
    assert.deepEqual(sourceDescriptor.populationCapacity.spotTypes[0], {
      objectKind: 'user_spot',
      stableType: 'lobby',
      active: 2,
      reserved: 0,
      limit: 64
    });
    const aggregateId = { value: '44444444-4444-4444-8444-444444444444' };
    const request = aggregateRequest(
      aggregateId,
      1n,
      snapshots,
      targetTarget
    );
    const prepareResults = await Promise.all([
      source.prepareAggregate(request),
      recovery.prepareAggregate(request)
    ]);
    assert.deepEqual(
      prepareResults.map(result => result.kind).sort(),
      ['alreadyPrepared', 'prepared']
    );
    const prepared = prepareResults.find(result =>
      result.kind === 'prepared'
    );
    assert.ok(prepared);
    if (prepared === undefined || prepared.kind !== 'prepared') return;
    assert.deepEqual(
      await recovery.prepareAggregate({
        ...request,
        capacity: userSpotCapacity(request.participants.length + 1)
      }),
      { kind: 'conflict' }
    );

    const hidden = await recovery.readAuthority(
      request.participants[0].authorityKey
    );
    assert.equal(hidden.kind, 'snapshot');
    assert.equal(hidden.ownerId, sourceOwner.token.ownerId);

    runAggregateCrashProcess(
      'commit-crash',
      91,
      fixture.url,
      prefix,
      prepared.fence
    );
    assert.deepEqual(
      await recovery.commitAggregate(prepared.fence),
      { kind: 'alreadyCommitted' }
    );
    assert.deepEqual(
      await source.commitAggregate(prepared.fence),
      { kind: 'alreadyCommitted' }
    );
    for (let index = 0; index < request.participants.length; index++) {
      const moved = await source.readAuthority(
        request.participants[index].authorityKey
      );
      assert.equal(moved.kind, 'snapshot');
      assert.equal(moved.ownerId, targetOwner.token.ownerId);
      assert.equal(moved.objectGeneration, snapshots[index].objectGeneration);
      assert.ok(
        moved.authorityOwnerGeneration
          > snapshots[index].authorityOwnerGeneration
      );
      assert.equal(moved.authorityOwnerGeneration, BigInt(index + 3));
      assert.equal(
        Buffer.from(moved.payload).toString(),
        `relocated-room-${index.toString().padStart(2, '0')}`
      );
    }
    const authorityOwnerCounter = await providerA.read({
      value: 'zlink:v11:authority-owner-counter'
    });
    assert.equal(authorityOwnerCounter.kind, 'found');
    assert.deepEqual(Buffer.from(authorityOwnerCounter.value.bytes), Buffer.from('5'));

    const abortSnapshot = await createReadyUserSpot(
      source,
      'room-abort',
      sourceTarget
    );
    const abortRequest = aggregateRequest(
      { value: '55555555-5555-4555-8555-555555555555' },
      1n,
      [abortSnapshot],
      targetTarget
    );
    const abortPrepared = await recovery.prepareAggregate(abortRequest);
    assert.equal(abortPrepared.kind, 'prepared');
    if (abortPrepared.kind !== 'prepared') return;
    runAggregateCrashProcess(
      'abort-crash',
      92,
      fixture.url,
      prefix,
      abortPrepared.fence
    );
    assert.deepEqual(
      await recovery.abortAggregate(abortPrepared.fence),
      { kind: 'alreadyAborted' }
    );
    const retained = await source.readAuthority(
      abortRequest.participants[0].authorityKey
    );
    assert.equal(retained.kind, 'snapshot');
    assert.equal(retained.ownerId, sourceOwner.token.ownerId);
    assert.equal(retained.storeVersion.value, abortSnapshot.storeVersion.value);
    const retainedRow = await providerA.read({
      value: authorityPreimage(abortRequest.participants[0].authorityKey)
    });
    assert.equal(retainedRow.kind, 'found');
    if (retainedRow.kind === 'found') {
      const retainedRecord = JSON.parse(
        Buffer.from(retainedRow.value.bytes).toString('utf8')
      );
      assert.equal(retainedRecord.aggregate, undefined);
    }

    const damagedSnapshot = await createReadyUserSpot(
      source,
      'room-damaged',
      sourceTarget
    );
    const damagedRequest = aggregateRequest(
      { value: '66666666-6666-4666-8666-666666666666' },
      1n,
      [damagedSnapshot],
      targetTarget,
      ['room-damaged']
    );
    const damagedPrepared = await source.prepareAggregate(damagedRequest);
    assert.equal(damagedPrepared.kind, 'prepared');
    if (damagedPrepared.kind !== 'prepared') return;
    const damagedPage = await firstInventoryPage(
      providerA,
      damagedPrepared.fence
    );
    await providerA.write({
      conditions: [{
        kind: 'version',
        key: damagedPage.key,
        expected: damagedPage.value.version
      }],
      mutations: [{
        kind: 'put',
        key: damagedPage.key,
        bytes: Buffer.from('damaged')
      }]
    });
    await assert.rejects(
      recovery.commitAggregate(damagedPrepared.fence),
      /checksum/
    );
    const afterDamage = await recovery.readAuthority(
      damagedRequest.participants[0].authorityKey
    );
    assert.equal(afterDamage.kind, 'snapshot');
    assert.equal(afterDamage.ownerId, sourceOwner.token.ownerId);
  } finally {
    await providerA.dispose();
    await providerB.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('aggregate committer retries a target owner-lease heartbeat conflict with unchanged authority', async () => {
  const inner = new frameworkInternal.ZLinkInMemoryProviderLocationStore();
  let targetRepository;
  let targetOwner;
  let heartbeatArmed = false;
  let heartbeatWrites = 0;
  let prepareCasAttempts = 0;
  const provider = {
    read: (key, signal) => inner.read(key, signal),
    scan: (request, signal) => inner.scan(request, signal),
    async write(request, signal) {
      const preparesAggregate = request.mutations.some(mutation =>
        mutation.kind === 'put'
        && mutation.key.value.startsWith('zlink:v11:aggregate:')
        && JSON.parse(Buffer.from(mutation.bytes).toString('utf8')).state === 'prepared'
      );
      if (preparesAggregate) prepareCasAttempts += 1;
      if (preparesAggregate && heartbeatArmed) {
        heartbeatArmed = false;
        const renewed = await targetRepository.renewOwnerLease(
          targetOwner.token,
          60_000,
          signal
        );
        assert.equal(renewed.kind, 'renewed');
        heartbeatWrites += 1;
      }
      return inner.write(request, signal);
    }
  };
  const source = new frameworkInternal.ZLinkLocationStoreRepository(provider);
  targetRepository = new frameworkInternal.ZLinkLocationStoreRepository(provider);
  const sourceOwner = await source.claimOwnerLease('heartbeat-source-owner', 60_000);
  targetOwner = await targetRepository.claimOwnerLease('heartbeat-target-owner', 60_000);
  assert.equal(sourceOwner.kind, 'claimed');
  assert.equal(targetOwner.kind, 'claimed');
  if (sourceOwner.kind !== 'claimed' || targetOwner.kind !== 'claimed') return;
  const sourceTarget = {
    meshName: 'heartbeat-play',
    nodeRid: 'heartbeat-source-node',
    nodeLifecycleGeneration: 1n,
    owner: sourceOwner.token
  };
  const targetTarget = {
    meshName: 'heartbeat-play',
    nodeRid: 'heartbeat-target-node',
    nodeLifecycleGeneration: 1n,
    owner: targetOwner.token
  };
  for (const [repository, target] of [[source, sourceTarget], [targetRepository, targetTarget]]) {
    const stored = await repository.updateMeshNode(
      aggregateDescriptor(target, 4),
      frameworkInternal.ZLinkLocationWriteIntent.NewClaim
    );
    assert.equal(stored.status, frameworkInternal.ZLinkLocationWriteStatus.Stored);
  }
  const authority = await createReadyUserSpot(
    source,
    'heartbeat-room',
    sourceTarget
  );
  const plan = aggregateCommitterPlan(
    'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa',
    authority,
    'heartbeat-room',
    targetTarget
  );
  heartbeatArmed = true;

  const prepared = await new frameworkInternal.ServiceRelocationAggregateCommitter(
    targetRepository
  ).prepare(plan);

  assert.equal(prepared.fence.aggregateId.value, plan.envelope.aggregateId);
  assert.equal(heartbeatWrites, 1);
  assert.equal(prepareCasAttempts, 2);
  const unchanged = await targetRepository.readAuthority(plan.participants[0].key);
  assert.equal(unchanged.kind, 'snapshot');
  assert.equal(unchanged.storeVersion.value, authority.storeVersion.value);
});

test('aggregate committer does not retry a conflict after authority StoreVersion changes', async () => {
  const key = encodeAuthorityKey('user_spot', 'changed-authority-room');
  const expected = {
    kind: 'snapshot',
    storeVersion: { value: 'source-v1' },
    payload: Buffer.from('source'),
    objectGeneration: 3n,
    authorityOwnerGeneration: 5n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 7n,
    allocation: {
      state: 'active',
      objectKind: 'user_spot',
      stableType: 'lobby',
      descriptor: { meshName: 'changed-play', rid: 'source-node' },
      descriptorLifecycleGeneration: 1n,
      capacity: userSpotCapacity(1)
    },
    storeNow: new Date()
  };
  const plan = aggregateCommitterPlan(
    'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb',
    expected,
    'changed-authority-room',
    {
      meshName: 'changed-play',
      nodeRid: 'target-node',
      nodeLifecycleGeneration: 2n,
      owner: { ownerId: 'target-owner', leaseGeneration: 11n }
    }
  );
  let attempts = 0;
  const store = {
    async prepareAggregate() {
      attempts += 1;
      return { kind: 'conflict' };
    },
    async readAuthority(readKey) {
      assert.equal(readKey.value, key.value);
      return { ...expected, storeVersion: { value: 'foreign-v2' } };
    },
    async commitAggregate() { return { kind: 'stale' }; },
    async abortAggregate() { return { kind: 'stale' }; }
  };

  await assert.rejects(
    new frameworkInternal.ServiceRelocationAggregateCommitter(store).prepare(plan),
    /aggregate prepare: conflict/
  );
  assert.equal(attempts, 1);
});

test('aggregate commit retries a target owner-lease heartbeat conflict while its fence stays prepared', async () => {
  const inner = new frameworkInternal.ZLinkInMemoryProviderLocationStore();
  let targetRepository;
  let targetOwner;
  let heartbeatArmed = false;
  let heartbeatWrites = 0;
  let commitCasAttempts = 0;
  const provider = {
    read: (key, signal) => inner.read(key, signal),
    scan: (request, signal) => inner.scan(request, signal),
    async write(request, signal) {
      const commitsAggregate = request.mutations.some(mutation =>
        mutation.kind === 'put'
        && mutation.key.value.startsWith('zlink:v11:aggregate:')
        && JSON.parse(Buffer.from(mutation.bytes).toString('utf8')).state === 'committed'
      );
      if (commitsAggregate) commitCasAttempts += 1;
      if (commitsAggregate && heartbeatArmed) {
        heartbeatArmed = false;
        const renewed = await targetRepository.renewOwnerLease(
          targetOwner.token,
          60_000,
          signal
        );
        assert.equal(renewed.kind, 'renewed');
        heartbeatWrites += 1;
      }
      return inner.write(request, signal);
    }
  };
  const source = new frameworkInternal.ZLinkLocationStoreRepository(provider);
  targetRepository = new frameworkInternal.ZLinkLocationStoreRepository(provider);
  const sourceOwner = await source.claimOwnerLease('commit-heartbeat-source-owner', 60_000);
  targetOwner = await targetRepository.claimOwnerLease('commit-heartbeat-target-owner', 60_000);
  assert.equal(sourceOwner.kind, 'claimed');
  assert.equal(targetOwner.kind, 'claimed');
  if (sourceOwner.kind !== 'claimed' || targetOwner.kind !== 'claimed') return;
  const sourceTarget = {
    meshName: 'commit-heartbeat-play',
    nodeRid: 'commit-heartbeat-source-node',
    nodeLifecycleGeneration: 1n,
    owner: sourceOwner.token
  };
  const targetTarget = {
    meshName: 'commit-heartbeat-play',
    nodeRid: 'commit-heartbeat-target-node',
    nodeLifecycleGeneration: 1n,
    owner: targetOwner.token
  };
  for (const [repository, target] of [[source, sourceTarget], [targetRepository, targetTarget]]) {
    const stored = await repository.updateMeshNode(
      aggregateDescriptor(target, 4),
      frameworkInternal.ZLinkLocationWriteIntent.NewClaim
    );
    assert.equal(stored.status, frameworkInternal.ZLinkLocationWriteStatus.Stored);
  }
  const authority = await createReadyUserSpot(
    source,
    'commit-heartbeat-room',
    sourceTarget
  );
  const request = aggregateRequest(
    { value: 'cccccccc-cccc-4ccc-8ccc-cccccccccccc' },
    1n,
    [authority],
    targetTarget,
    ['commit-heartbeat-room']
  );
  const prepared = await targetRepository.prepareAggregate(request);
  assert.equal(prepared.kind, 'prepared');
  if (prepared.kind !== 'prepared') return;
  heartbeatArmed = true;

  const committed = await targetRepository.commitAggregate(prepared.fence);

  assert.equal(committed.kind, 'committed');
  assert.equal(heartbeatWrites, 1);
  assert.equal(commitCasAttempts, 2);
  const moved = await targetRepository.readAuthority(request.participants[0].authorityKey);
  assert.equal(moved.kind, 'snapshot');
  assert.equal(moved.ownerId, targetOwner.token.ownerId);
  assert.equal(moved.ownerLeaseGeneration, targetOwner.token.leaseGeneration);
});

test('in-memory aggregate prepare, commit, and abort converge across repository instances', async () => {
  const provider = new frameworkInternal.ZLinkInMemoryProviderLocationStore();
  const source = new frameworkInternal.ZLinkLocationStoreRepository(provider);
  const recovery = new frameworkInternal.ZLinkLocationStoreRepository(provider);
  const sourceOwner = await source.claimOwnerLease('memory-source-owner', 60_000);
  const targetOwner = await recovery.claimOwnerLease('memory-target-owner', 60_000);
  assert.equal(sourceOwner.kind, 'claimed');
  assert.equal(targetOwner.kind, 'claimed');
  if (sourceOwner.kind !== 'claimed' || targetOwner.kind !== 'claimed') return;
  const sourceTarget = {
    meshName: 'memory-play', nodeRid: 'memory-source-node', nodeLifecycleGeneration: 1n,
    owner: sourceOwner.token
  };
  const targetTarget = {
    meshName: 'memory-play', nodeRid: 'memory-target-node', nodeLifecycleGeneration: 1n,
    owner: targetOwner.token
  };
  for (const [repository, target] of [[source, sourceTarget], [recovery, targetTarget]]) {
    const stored = await repository.updateMeshNode(
      aggregateDescriptor(target, 16), frameworkInternal.ZLinkLocationWriteIntent.NewClaim
    );
    assert.equal(stored.status, frameworkInternal.ZLinkLocationWriteStatus.Stored);
  }
  const snapshots = [];
  for (let index = 0; index < 4; index++) {
    snapshots.push(await createReadyUserSpot(source, `memory-room-${index}`, sourceTarget));
  }
  const request = aggregateRequest(
    { value: '77777777-7777-4777-8777-777777777777' }, 1n, snapshots, targetTarget,
    snapshots.map((_, index) => `memory-room-${index}`)
  );
  const prepares = await Promise.all([
    source.prepareAggregate(request), recovery.prepareAggregate(request)
  ]);
  assert.deepEqual(prepares.map(value => value.kind).sort(), ['alreadyPrepared', 'prepared']);
  const prepared = prepares.find(value => value.kind === 'prepared');
  assert.ok(prepared);
  if (prepared === undefined || prepared.kind !== 'prepared') return;
  const commits = await Promise.all([
    source.commitAggregate(prepared.fence), recovery.commitAggregate(prepared.fence)
  ]);
  assert.equal(commits.every(value =>
    value.kind === 'committed' || value.kind === 'alreadyCommitted'), true);

  const abortSnapshot = await createReadyUserSpot(source, 'memory-room-abort', sourceTarget);
  const abortRequest = aggregateRequest(
    { value: '88888888-8888-4888-8888-888888888888' }, 1n,
    [abortSnapshot], targetTarget, ['memory-room-abort']
  );
  const abortPrepared = await source.prepareAggregate(abortRequest);
  assert.equal(abortPrepared.kind, 'prepared');
  if (abortPrepared.kind !== 'prepared') return;
  const aborts = await Promise.all([
    source.abortAggregate(abortPrepared.fence), recovery.abortAggregate(abortPrepared.fence)
  ]);
  assert.deepEqual(aborts.map(value => value.kind).sort(), ['aborted', 'alreadyAborted']);

  const foreignSnapshot = await createReadyUserSpot(
    source, 'memory-room-foreign-source', sourceTarget
  );
  const foreignRequest = aggregateRequest(
    { value: '99999999-9999-4999-8999-999999999999' },
    1n,
    [foreignSnapshot],
    targetTarget,
    ['memory-room-foreign-source']
  );
  const foreignPrepared = await recovery.prepareAggregate(foreignRequest);
  assert.equal(foreignPrepared.kind, 'prepared');
  if (foreignPrepared.kind !== 'prepared') return;
  const foreignSourceCapacityKey = {
    value: 'zlink:v11:capacity:memory-play:memory-source-node'
  };
  const foreignSourceCapacity = await provider.read(foreignSourceCapacityKey);
  assert.equal(foreignSourceCapacity.kind, 'found');
  if (foreignSourceCapacity.kind !== 'found') return;
  assert.equal((await provider.write({
    conditions: [{
      kind: 'version',
      key: foreignSourceCapacityKey,
      expected: foreignSourceCapacity.value.version
    }],
    mutations: [{ kind: 'delete', key: foreignSourceCapacityKey }]
  })).kind, 'applied');
  assert.deepEqual(
    await recovery.commitAggregate(foreignPrepared.fence),
    { kind: 'committed' }
  );
  const foreignMoved = await recovery.readAuthority(
    foreignRequest.participants[0].authorityKey
  );
  assert.equal(foreignMoved.kind, 'snapshot');
  assert.equal(foreignMoved.ownerId, targetOwner.token.ownerId);
});

test('redis opaque Relocation Store uses Framework-issued immutable references', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const prefix = testPrefix('relocation');
  const store = new redisLocations.ZLinkRedisRelocationStore({
    url: fixture.url,
    keyPrefix: prefix
  });
  const blob = reference('framework-issued-reference');
  const payload = Uint8Array.from([0, 1, 2, 255]);

  try {
    const stored = await store.put(blob, payload, 60_000);
    assert.equal(stored.kind, 'stored');
    assert.equal(stored.expiresAt > stored.storeNow, true);
    const duplicate = await store.put(blob, payload, 60_000);
    assert.equal(duplicate.kind, 'alreadyStored');
    const conflict = await store.put(blob, Buffer.from('different'), 60_000);
    assert.equal(conflict.kind, 'conflict');

    const found = await store.read(blob);
    assert.equal(found.kind, 'found');
    assert.deepEqual([...found.bytes], [...payload]);
    const renewed = await store.renew(blob, 120_000);
    assert.equal(renewed.kind, 'renewed');
    assert.equal(renewed.expiresAt > renewed.storeNow, true);

    await store.delete(blob);
    await store.delete(blob);
    assert.equal((await store.read(blob)).kind, 'missing');
    assert.equal((await store.renew(blob, 10_000)).kind, 'missing');
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('redis Store validates exact public bounds', async () => {
  assert.throws(
    () => new redisLocations.ZLinkRedisLocationStore({
      url: 'redis://127.0.0.1:6379',
      keyPrefix: ''
    }),
    /keyPrefix/
  );
  const location = new redisLocations.ZLinkRedisLocationStore({
    url: 'redis://127.0.0.1:6379',
    keyPrefix: 'bounds'
  });
  await assert.rejects(
    location.write({
      conditions: [],
      mutations: [{ kind: 'put', key: key('large'), bytes: new Uint8Array(1024 * 1024 + 1) }]
    }),
    /1 MiB/
  );
  await assert.rejects(location.scan({ prefix: '', limit: 0 }), /1..1000/);
  await location.dispose();
});

test('redis-backed repository writes the golden canonical authority allocation envelope', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const golden = JSON.parse(fs.readFileSync(
    path.join(
      __dirname, '..', '..', '..', '..',
      'runtime', 'protocol', 'golden', 'store-record-v1.json'
    ),
    'utf8'
  ));
  const vector = golden.valueVectors.genericOpaqueRecord.find(
    item => item.name === 'authority-actor-normal'
  );
  assert.ok(vector);
  const prefix = testPrefix('golden-conformance');
  const store = new redisLocations.ZLinkRedisLocationStore({ url: fixture.url, keyPrefix: prefix });
  const repository = new frameworkInternal.ZLinkLocationStoreRepository(store);
  try {
    const owner = await repository.claimOwnerLease('owner-a', 60_000);
    assert.equal(owner.kind, 'claimed');
    if (owner.kind !== 'claimed') throw new Error('owner lease claim failed');
    const routingId = {
      toHex: () => vector.decoded.allocation.descriptor.routingIdHex,
      toString: () => vector.decoded.allocation.descriptor.routingIdHex
    };
    const target = {
      meshName: vector.decoded.allocation.descriptor.meshName,
      nodeRid: routingId,
      nodeLifecycleGeneration: 1n,
      owner: owner.token
    };
    const descriptor = aggregateDescriptor(target, 0);
    descriptor.populationCapacity.actors.limit = vector.decoded.allocation.capacity.actors;
    descriptor.objectCapabilities = [{
      objectKind: 'actor', stableType: vector.decoded.allocation.stableType,
      policy: 'snapshot', hasSnapshotAdapter: true, limit: vector.decoded.allocation.capacity.actors
    }];
    assert.equal(
      (await repository.updateMeshNode(
        descriptor,
        frameworkInternal.ZLinkLocationWriteIntent.NewClaim
      )).status,
      frameworkInternal.ZLinkLocationWriteStatus.Stored
    );
    const objectCounter = golden.counterVectors.vectors.find(
      item => item.logicalKey === 'zlink:v11:object-counter'
    );
    const authorityOwnerCounter = golden.counterVectors.vectors.find(
      item => item.logicalKey === 'zlink:v11:authority-owner-counter'
    );
    assert.ok(objectCounter);
    assert.ok(authorityOwnerCounter);
    const seededCounters = await store.write({
      conditions: [
        { kind: 'missing', key: key(objectCounter.logicalKey) },
        { kind: 'missing', key: key(authorityOwnerCounter.logicalKey) }
      ],
      mutations: [
        { kind: 'put', key: key(objectCounter.logicalKey), bytes: Buffer.from(objectCounter.issuedValue) },
        {
          kind: 'put',
          key: key(authorityOwnerCounter.logicalKey),
          bytes: Buffer.from(authorityOwnerCounter.issuedValue)
        }
      ]
    });
    assert.equal(seededCounters.kind, 'applied');
    const authorityKey = encodeAuthorityKey('actor', 'user:42');
    const reserved = await repository.reserve({
      key: { kind: 'actor', globalId: 'user:42' },
      intent: {
        stableType: vector.decoded.allocation.stableType,
        requestContentReference: 'golden-authority-request',
        requestSha256: Buffer.alloc(32, 1),
        requestEncodedSize: 16n
      },
      target,
      creatingPayload: Buffer.from(vector.decoded.payload, 'base64'),
      capacity: { actors: vector.decoded.allocation.capacity.actors, spots: 0 }
    });
    assert.equal(reserved.kind, 'reserved');
    if (reserved.kind !== 'reserved') throw new Error('authority reserve failed');
    assert.equal(reserved.creating.objectGeneration, BigInt(objectCounter.issuedValue));
    assert.equal(
      reserved.creating.authorityOwnerGeneration,
      BigInt(authorityOwnerCounter.issuedValue)
    );
    for (const counter of [objectCounter, authorityOwnerCounter]) {
      const read = await store.read(key(counter.logicalKey));
      assert.equal(read.kind, 'found');
      assert.deepEqual(
        Buffer.from(read.value.bytes),
        Buffer.from(counter.storedNextValue),
        `${counter.logicalKey} must retain the golden bare-decimal next value`
      );
    }
    const committed = await repository.commit({
      key: { kind: 'actor', globalId: 'user:42' },
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target,
      readyPayload: Buffer.from(vector.decoded.payload, 'base64')
    });
    assert.equal(committed.kind, 'committed');

    // Read the opaque row written by reserve()/commit(), rather than injecting
    // fixture bytes into the provider. Dynamic Store-wide generations and the
    // lease token are intentionally compared by type; every fixed envelope
    // field must match the authoritative golden allocation exactly.
    const read = await store.read(key(authorityPreimage(authorityKey)));
    assert.equal(read.kind, 'found');
    const stored = JSON.parse(Buffer.from(read.value.bytes).toString('utf8'));
    const canonicalStored = Object.fromEntries(
      Object.keys(vector.decoded).map((field) => [field, stored[field]])
    );
    assert.deepEqual(canonicalStored, {
      ...vector.decoded,
      objectGeneration: String(stored.objectGeneration),
      authorityOwnerGeneration: String(stored.authorityOwnerGeneration),
      ownerLeaseGeneration: String(stored.ownerLeaseGeneration)
    });
    assert.match(stored.objectGeneration, /^\d+$/);
    assert.match(stored.authorityOwnerGeneration, /^\d+$/);
    assert.match(stored.ownerLeaseGeneration, /^\d+$/);
    assert.equal('target' in stored.allocation, false);
    assert.equal('capacityBundle' in stored.allocation, false);
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('redis-backed production repository writes the golden canonical descriptor bodies', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const golden = JSON.parse(fs.readFileSync(path.join(
    __dirname, '..', '..', '..', '..', 'runtime', 'protocol', 'golden', 'store-record-v1.json'
  ), 'utf8'));
  const prefix = testPrefix('golden-descriptor-conformance');
  const store = new redisLocations.ZLinkRedisLocationStore({ url: fixture.url, keyPrefix: prefix });
  const repository = new frameworkInternal.ZLinkLocationStoreRepository(store);
  try {
    const owner = await repository.claimOwnerLease('owner-a', 60_000);
    assert.equal(owner.kind, 'claimed');
    if (owner.kind !== 'claimed') throw new Error('owner lease claim failed');
    for (const [name, update] of [
      ['meshNodeDescriptor-normal', 'updateMeshNode'],
      ['clientServerDescriptor-normal', 'updateClientServer'],
      ['fanoutPublisherDescriptor-normal', 'updateFanoutPublisher']
    ]) {
      const vector = golden.valueVectors.genericOpaqueRecord.find(item => item.name === name);
      assert.ok(vector, `${name} fixture missing`);
      const body = vector.decoded.descriptor;
      const rid = { toHex: () => body.routingIdHex ?? body.serverRoutingIdHex ?? body.publisherRoutingIdHex,
        toString: () => body.routingIdHex ?? body.serverRoutingIdHex ?? body.publisherRoutingIdHex };
      const common = {
        lifecycleGeneration: BigInt(body.lifecycleGeneration), descriptorRevision: BigInt(body.descriptorRevision),
        endpoint: body.endpoint, state: framework.ZLinkFrameworkRuntimeState.Serving,
        securityIdentity: body.securityIdentity, ownerId: owner.token.ownerId,
        leaseGeneration: owner.token.leaseGeneration, updatedAt: new Date(Number(body.updatedAtEpochMs))
      };
      const descriptor = update === 'updateMeshNode' ? {
        ...common, meshName: body.meshName, rid, objectRole: framework.ZLinkObjectRole.Server,
        entrySpotId: body.entrySpotId, channelWeights: body.channelWeights,
        applicationVersion: BigInt(body.applicationVersion), placementWeight: body.placementWeight,
        populationCapacity: { ...body.capacity, spotTypes: body.capacity.spotTypes.map(value => ({ ...value,
          objectKind: value.objectKind === 'userSpot' ? 'user_spot' : 'instance_spot' })) },
        activationConcurrency: body.activationConcurrency,
        spotTypes: [], objectCapabilities: body.objectCapabilities.map(value => ({ ...value,
          objectKind: value.objectKind === 'userSpot' ? 'user_spot' : value.objectKind === 'instanceSpot' ? 'instance_spot' : 'actor' })),
        maintenanceWave: body.maintenanceWave ?? undefined
      } : update === 'updateClientServer' ? { ...common, channelName: body.channelName, serverRid: rid, weight: body.weight }
        : { ...common, channelName: body.channelName, publisherRid: rid };
      const result = await repository[update](descriptor, frameworkInternal.ZLinkLocationWriteIntent.NewClaim);
      assert.equal(result.status, frameworkInternal.ZLinkLocationWriteStatus.Stored);
      const read = await store.read(key(vector.originalKey.replace(/\\u0000/g, '\0')));
      assert.equal(read.kind, 'found', name);
      const stored = JSON.parse(Buffer.from(read.value.bytes).toString('utf8'));
      assert.deepEqual(stored, {
        ...vector.decoded,
        leaseGeneration: String(owner.token.leaseGeneration),
        ownerId: owner.token.ownerId,
        descriptor: { ...vector.decoded.descriptor, leaseGeneration: String(owner.token.leaseGeneration), ownerId: owner.token.ownerId }
      });
    }
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

test('redis-backed production repository rejects an authority envelope without recordVersion', async (t) => {
  const fixture = await redisFixture(t);
  if (fixture === undefined) return;
  const prefix = testPrefix('record-version');
  const store = new redisLocations.ZLinkRedisLocationStore({ url: fixture.url, keyPrefix: prefix });
  const repository = new frameworkInternal.ZLinkLocationStoreRepository(store);
  const authorityKey = encodeAuthorityKey('actor', 'legacy-record');
  try {
    const written = await store.write({
      conditions: [{ kind: 'missing', key: key(authorityPreimage(authorityKey)) }],
      mutations: [{
        kind: 'put',
        key: key(authorityPreimage(authorityKey)),
        bytes: Buffer.from(JSON.stringify({ snapshot: {} }))
      }]
    });
    assert.equal(written.kind, 'applied');
    await assert.rejects(
      repository.readAuthority(authorityKey),
      /unrecognized recordVersion/
    );
  } finally {
    await store.dispose();
    await cleanup(fixture.client, prefix);
    await fixture.client.quit();
  }
});

function aggregateDescriptor(target, limit) {
  return {
    meshName: target.meshName,
    rid: target.nodeRid,
    lifecycleGeneration: target.nodeLifecycleGeneration,
    descriptorRevision: 1n,
    endpoint: `tcp://${target.nodeRid}`,
    objectRole: framework.ZLinkObjectRole.Server,
    placementWeight: 100,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: 0 },
      spots: { active: 0, reserved: 0, limit },
      spotTypes: [{
        objectKind: 'user_spot',
        stableType: 'lobby',
        active: 0,
        reserved: 0,
        limit
      }]
    },
    activationConcurrency: { active: 0, limit },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: ['lobby'],
    objectCapabilities: [{
      objectKind: 'user_spot',
      stableType: 'lobby',
      policy: 'snapshot',
      hasSnapshotAdapter: true,
      limit
    }],
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: String(target.nodeRid),
    ownerId: target.owner.ownerId,
    leaseGeneration: target.owner.leaseGeneration,
    updatedAt: new Date()
  };
}

async function createReadyUserSpot(repository, spotId, target) {
  const capacity = userSpotCapacity(1);
  const reserved = await repository.reserve({
    key: { kind: 'user_spot', globalId: spotId },
    intent: {
      stableType: 'lobby',
      requestContentReference: `request:${spotId}`,
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 16n
    },
    target,
    creatingPayload: Buffer.from(`creating-${spotId}`),
    capacity
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') throw new Error('reservation failed');
  const committed = await repository.commit({
    key: { kind: 'user_spot', globalId: spotId },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target,
    readyPayload: Buffer.from(`ready-${spotId}`)
  });
  assert.equal(committed.kind, 'committed');
  if (committed.kind !== 'committed') throw new Error('commit failed');
  return committed.ready;
}

function aggregateRequest(
  aggregateId,
  generation,
  snapshots,
  target,
  spotIds = snapshots.map((_, index) =>
    snapshots.length === 1
      ? 'room-abort'
      : `room-${index.toString().padStart(2, '0')}`)
) {
  const participants = snapshots.map((snapshot, index) => {
    const spotId = spotIds[index];
    return {
      authorityKey: encodeAuthorityKey('user_spot', spotId),
      expectedStoreVersion: snapshot.storeVersion,
      ownerTransition: 'newOwner',
      authorityPayload: Buffer.from(`relocated-${spotId}`),
      membershipMutation: Buffer.from(`membership-${spotId}`)
    };
  }).sort((left, right) =>
    left.authorityKey.value.localeCompare(right.authorityKey.value));
  return {
    aggregateId,
    aggregateGeneration: generation,
    participants,
    inventoryDigest: Buffer.alloc(32, 9),
    targetDescriptor: {
      meshName: target.meshName,
      rid: target.nodeRid
    },
    targetDescriptorLifecycleGeneration: target.nodeLifecycleGeneration,
    capacity: userSpotCapacity(snapshots.length),
    targetOwner: target.owner
  };
}

function aggregateCommitterPlan(aggregateId, snapshot, spotId, target) {
  const key = encodeAuthorityKey('user_spot', spotId);
  return {
    envelope: {
      aggregateId,
      aggregateGeneration: 1n,
      participants: [{
        key: key.value,
        objectKind: 'user_spot',
        stableType: 'lobby',
        objectGeneration: snapshot.objectGeneration,
        authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
        applicationState: Buffer.alloc(0),
        boundSessionState: Buffer.alloc(0),
        queuedMessages: [],
        timers: []
      }],
      memberships: []
    },
    participants: [{
      key,
      expected: snapshot,
      ownerTransition: 'newOwner',
      authorityPayload: Buffer.from(`relocated-${spotId}`),
      membershipMutation: Buffer.alloc(0)
    }],
    targetDescriptor: { meshName: target.meshName, rid: target.nodeRid },
    targetDescriptorLifecycleGeneration: target.nodeLifecycleGeneration,
    capacity: userSpotCapacity(1),
    targetOwner: target.owner
  };
}

async function firstInventoryPage(provider, fence) {
  const prefix =
    `zlink:v11:aggregate-inventory:${fence.aggregateId.value}:`
    + `${fence.aggregateGeneration}:page:`;
  const result = await provider.scan({ prefix, limit: 1 });
  assert.equal(result.kind, 'page');
  assert.equal(result.value.items.length, 1);
  return result.value.items[0];
}

function runAggregateCrashProcess(
  operation,
  expectedExitCode,
  url,
  keyPrefix,
  fence
) {
  const fixture = path.join(
    __dirname,
    'fixtures',
    'redis-aggregate-process.js'
  );
  const result = spawnSync(process.execPath, [
    fixture,
    operation,
    url,
    keyPrefix,
    fence.aggregateId.value,
    fence.aggregateGeneration.toString()
  ], {
    cwd: path.resolve(__dirname, '../..'),
    encoding: 'utf8',
    timeout: 30_000
  });
  assert.equal(result.status, expectedExitCode, result.stderr);
}

function userSpotCapacity(count) {
  return {
    actors: 0,
    spots: count,
    spotType: {
      objectKind: 'user_spot',
      stableType: 'lobby',
      count
    }
  };
}

async function redisFixture(t) {
  const candidates = [
    process.env.ZLINK_REDIS_TEST_ENDPOINT,
    '127.0.0.1:16379',
    '127.0.0.1:6379'
  ].filter(Boolean);
  for (const endpoint of candidates) {
    const url = endpoint.startsWith('redis://') ? endpoint : `redis://${endpoint}`;
    const client = createClient({
      url,
      socket: { connectTimeout: 300, reconnectStrategy: false }
    });
    client.on('error', () => {});
    try {
      await Promise.race([
        client.connect(),
        new Promise((_, reject) =>
          setTimeout(() => reject(new Error('Redis probe timeout')), 500)
        )
      ]);
      await client.ping();
      return { url, client };
    } catch {
      try {
        if (client.isOpen) await client.disconnect();
      } catch {}
    }
  }
  t.skip('Redis is not reachable.');
  return undefined;
}

async function cleanup(client, prefix) {
  let cursorValue = '0';
  do {
    const page = await client.scan(cursorValue, {
      MATCH: `${prefix}:*`,
      COUNT: 100
    });
    cursorValue = String(page.cursor);
    if (page.keys.length > 0) await client.del(page.keys);
  } while (cursorValue !== '0');
}

function testPrefix(scope) {
  return `zlink:opaque:${scope}:${process.pid}:${Date.now()}`;
}

function key(value) {
  return { value };
}

function version(value) {
  return { value };
}

function cursor(value) {
  return { value };
}

function reference(value) {
  return { value };
}
