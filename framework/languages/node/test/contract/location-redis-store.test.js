const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');
const { createClient } = require('redis');
const framework = require('../../packages/framework/dist');
const redisLocations = require('../../packages/framework-locations-redis/dist');
const frameworkInternal = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

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
      cursor: cursor('00000000-0000-0000-0000-000000000000:0'),
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
    for (let index = 0; index < 12; index++) {
      snapshots.push(await createReadyUserSpot(
        source,
        `room-${index.toString().padStart(2, '0')}`,
        sourceTarget
      ));
    }
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
      assert.equal(
        Buffer.from(moved.payload).toString(),
        `relocated-room-${index.toString().padStart(2, '0')}`
      );
    }

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
      value: `zlink:v11:authority:${
        encodeURIComponent(abortRequest.participants[0].authorityKey.value)
      }`
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
