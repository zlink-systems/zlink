'use strict';

const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const test = require('node:test');

const { ZLinkActorPlacementCoordinator } = require(
  '../../packages/framework/dist/runtime/host/actor-placement-coordinator'
);
const { encodeLocationCreationContent } = require(
  '../../packages/framework/dist/runtime/host/user-spot-creation-coordinator'
);

test('remote Actor factory failure aborts the exact authority capacity reservation', async () => {
  const requestPayload = Buffer.from('{}');
  const snapshot = {
    kind: 'snapshot',
    objectGeneration: 1n,
    authorityOwnerGeneration: 1n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 3n,
    storeVersion: { value: 'version-1' },
    payload: Buffer.alloc(0),
    allocation: {
      state: 'reserved',
      objectKind: 'actor',
      stableType: 'Config6Actor',
      descriptor: { meshName: 'profile', rid: 'api-a' },
      descriptorLifecycleGeneration: 9n,
      capacity: { actors: 1, spots: 0 }
    },
    pendingCreation: {
      reservationId: 'reservation-1',
      requestContentReference: encodeLocationCreationContent(requestPayload),
      requestSha256: createHash('sha256').update(requestPayload).digest(),
      requestEncodedSize: BigInt(requestPayload.length)
    }
  };
  const aborts = [];
  const coordinator = new ZLinkActorPlacementCoordinator({
    store: {
      async readAuthority() { return snapshot; },
      async abort(request) { aborts.push(request); return { kind: 'aborted' }; }
    }
  });
  const record = {
    actorId: 'failed',
    stableType: 'Config6Actor',
    correlation: 4n,
    operation: { high: 1n, low: 2n },
    sourceNodeRid: 'caller',
    sourceNodeGeneration: 1n,
    deadlineUnixMs: BigInt(Date.now() + 1000),
    reservation: {
      reservationId: 'reservation-1',
      expectedStoreVersion: 'version-1',
      objectGeneration: 1n,
      authorityOwnerGeneration: 1n,
      targetNodeRid: 'api-a',
      targetNodeGeneration: 9n,
      targetOwnerId: 'owner-a',
      targetOwnerLeaseGeneration: 3n,
      pendingCapacityDelta: 1
    }
  };

  await assert.rejects(
    () => coordinator.handleRemoteCreate(
      record,
      async () => { throw new Error('factory failed'); },
      new AbortController().signal
    ),
    /factory failed/
  );

  assert.deepEqual(aborts, [{
    key: { kind: 'actor', globalId: 'failed' },
    reservationId: 'reservation-1',
    expectedStoreVersion: 'version-1',
    target: {
      meshName: 'profile',
      nodeRid: 'api-a',
      nodeLifecycleGeneration: 9n,
      owner: { ownerId: 'owner-a', leaseGeneration: 3n }
    }
  }]);
});
