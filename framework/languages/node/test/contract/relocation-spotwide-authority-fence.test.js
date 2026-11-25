const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { userSpotCapacity } = require('./helpers/location-aggregate-fixture');

const digest = 'a'.repeat(64);
const relocationId = '00000000-0000-0000-0000-000000000001';
const source = {
  nodeRid: 'source',
  nodeGeneration: 5n,
  ownerId: 'source-owner',
  ownerLeaseGeneration: 3n
};
const spotKey = encodeAuthorityKey('user_spot', 'room');
const actorKey = encodeAuthorityKey('actor', 'player');

function fixture() {
  const aggregateRequests = [];
  const makeAuthority = (objectKind, stableType, version, capacity) => ({
    kind: 'snapshot',
    storeVersion: { value: version },
    objectGeneration: 7n,
    authorityOwnerGeneration: 11n,
    ownerId: source.ownerId,
    ownerLeaseGeneration: source.ownerLeaseGeneration,
    payload: Buffer.alloc(0),
    allocation: {
      state: 'active',
      objectKind,
      stableType,
      descriptor: { meshName: 'mesh', rid: source.nodeRid },
      descriptorLifecycleGeneration: source.nodeGeneration,
      capacity
    }
  });
  const spot = makeAuthority('user_spot', 'lobby', 'spot-sealed', userSpotCapacity(1));
  const actor = makeAuthority('actor', 'player', 'member-sealed', { actors: 1, spots: 0 });
  const rows = new Map([
    [spotKey.value, spot],
    [actorKey.value, actor]
  ]);
  const store = {
    async readAuthority(key) {
      return rows.get(key.value);
    },
    async prepareAggregate(request) {
      aggregateRequests.push(request);
      for (const participant of request.participants) {
        if (
          rows.get(participant.authorityKey.value).storeVersion.value !==
          participant.expectedStoreVersion.value
        ) {
          return { kind: 'conflict' };
        }
      }
      return {
        kind: 'prepared',
        fence: {
          aggregateId: request.aggregateId,
          aggregateGeneration: request.aggregateGeneration
        }
      };
    }
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => store,
    meshNode: () => ({
      status: () => ({ routingId: source.nodeRid, lifecycleGeneration: source.nodeGeneration })
    }),
    spotManager: () => ({ entrySpotIdForMesh: () => 'target-entry' })
  });
  const memberships = [
    {
      actorKey: actorKey.value,
      spotKey: spotKey.value,
      spotObjectGeneration: spot.objectGeneration,
      membershipEpoch: 1n
    }
  ];
  const prepare = {
    coordinator: {
      ownerId: source.ownerId,
      leaseGeneration: source.ownerLeaseGeneration,
      nodeRid: source.nodeRid,
      nodeGeneration: source.nodeGeneration,
      expectedAuthorityStoreVersion: spot.storeVersion.value
    },
    sourceNodeRid: source.nodeRid,
    sourceNodeGeneration: source.nodeGeneration,
    applicationVersion: 0n,
    target: {
      ownerId: 'target-owner',
      ownerLeaseGeneration: 13n,
      nodeRid: 'target',
      nodeGeneration: 17n
    },
    targetAttemptGeneration: 1n,
    payloadChecksumCrc32c: 0
  };
  const wireSource = { ...source, nodeRid: Buffer.from(source.nodeRid).toString('hex') };
  function seedRepresentativeCapture() {
    rows.set(spotKey.value, {
      ...spot,
      storeVersion: { value: 'spot-captured' },
      relocation: {
        relocationId,
        phase: 'captured',
        source: wireSource,
        target: null,
        coordinator: { ...wireSource, expectedStoreVersion: spot.storeVersion.value },
        sourceInventorySha256: digest,
        applicationVersion: 0n
      }
    });
  }
  const envelope = {
    aggregateId: relocationId,
    aggregateGeneration: 1n,
    participants: [
      [spotKey, spot],
      [actorKey, actor]
    ].map(([key, snapshot]) => ({
      objectKind: snapshot.allocation.objectKind,
      stableType: snapshot.allocation.stableType,
      objectGeneration: snapshot.objectGeneration,
      authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
      key: key.value,
      applicationState: Buffer.alloc(0),
      boundSessionState: Buffer.alloc(0),
      queuedMessages: [],
      timers: []
    })),
    memberships
  };
  const entries = [
    [actorKey, actor],
    [spotKey, spot]
  ].map(([key, snapshot]) => ({
    authorityKey: key.value,
    expectedStoreVersion: snapshot.storeVersion.value,
    ownerTransition: 'newOwner',
    authorityPayload: snapshot.payload,
    membershipMutation: Buffer.from('[]')
  }));
  return {
    actor,
    entries,
    spot,
    rows,
    runtime,
    prepare,
    envelope,
    aggregateRequests,
    seedRepresentativeCapture
  };
}

test('SpotWide PREPARE accepts a Captured representative and a steady member', async () => {
  const f = fixture();
  f.seedRepresentativeCapture();
  await f.runtime.reserveTargetForPrepare('mesh', f.prepare, f.envelope, f.entries, digest);
  const member = f.aggregateRequests[0].participants.find(
    (p) => p.authorityKey.value === actorKey.value
  );
  assert.deepEqual(member.expectedStoreVersion, f.actor.storeVersion);
});

test('SpotWide PREPARE rejects a version-only member write after seal', async () => {
  const f = fixture();
  f.seedRepresentativeCapture();
  f.rows.set(actorKey.value, { ...f.actor, storeVersion: { value: 'member-written-after-seal' } });
  await assert.rejects(
    f.runtime.reserveTargetForPrepare('mesh', f.prepare, f.envelope, f.entries, digest)
  );
  assert.equal(f.rows.get(actorKey.value).ownerId, source.ownerId);
  assert.equal(f.rows.get(spotKey.value).ownerId, source.ownerId);
});

for (const [field, value] of [
  ['ownerId', 'other-owner'],
  ['leaseGeneration', 31n],
  ['expectedAuthorityStoreVersion', 'other-baseline']
]) {
  test(`SpotWide PREPARE rejects a changed representative coordinator ${field}`, async () => {
    const f = fixture();
    f.seedRepresentativeCapture();
    f.prepare.coordinator[field] = value;
    await assert.rejects(
      f.runtime.reserveTargetForPrepare('mesh', f.prepare, f.envelope, f.entries, digest),
      /authority changed/
    );
    assert.equal(f.aggregateRequests.length, 0);
    assert.equal(f.rows.get(spotKey.value).ownerId, source.ownerId);
    assert.equal(f.rows.get(actorKey.value).ownerId, source.ownerId);
  });
}
