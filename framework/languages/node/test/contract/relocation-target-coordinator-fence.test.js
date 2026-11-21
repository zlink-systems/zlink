const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  encodeActorAuthorityIdentity
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  ServiceRelocationAuthorityPayloadCodec,
  encodeCanonicalRelocationSlot
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');
const {
  decodeCanonicalAuthorityPayload,
  encodeCanonicalAuthorityPayload
} = require('../../packages/framework/dist/runtime/actors/actor-authority-payload-codec');

for (const recorded of [false, true]) {
  for (const changed of ['none', 'owner', 'lease', 'baseline']) {
    test(`target rejects changed coordinator fence (${recorded ? 'captured' : 'steady'}, ${changed})`, async () => {
      const aggregateId = '00000000-0000-0000-0000-000000000001';
      const key = encodeAuthorityKey('actor', 'actor-a').value;
      const coordinator = {
        ownerId: 'source-owner',
        leaseGeneration: 3n,
        nodeRid: 'source',
        nodeGeneration: 5n,
        expectedAuthorityStoreVersion: 'baseline'
      };
      let payload = encodeActorAuthorityIdentity({
        actorType: 'Player',
        actor: { actorId: 'actor-a', objectGeneration: 7n, meshName: 'mesh', nodeRid: 'source' },
        meshName: 'mesh',
        ownerNodeGeneration: 5n,
        owner: { ownerId: 'source-owner', leaseGeneration: 3n },
        spotId: 'entry',
        spotGeneration: 5n,
        spotKind: 'entry'
      });
      if (recorded) {
        const slot = encodeCanonicalRelocationSlot({
          reference: 'pending',
          checksumCrc32c: 0,
          aggregateId,
          aggregateGeneration: 1n,
          inventoryDigest: '0'.repeat(64),
          targetOwnerId: '',
          targetOwnerLeaseGeneration: 0n,
          targetNodeRid: '',
          targetNodeGeneration: 0n,
          targetAttemptGeneration: 0n,
          coordinatorOwnerId: coordinator.ownerId,
          coordinatorLeaseGeneration: coordinator.leaseGeneration,
          coordinatorNodeRid: coordinator.nodeRid,
          coordinatorNodeGeneration: coordinator.nodeGeneration,
          sourceNodeRid: 'source',
          sourceNodeGeneration: 5n,
          sourceOwnerId: 'source-owner',
          sourceOwnerLeaseGeneration: 3n,
          coordinatorExpectedStoreVersion: 'baseline',
          phase: 2,
          applicationVersion: 0n,
          sourceCleanupState: 0
        });
        const body = decodeCanonicalAuthorityPayload(payload).body;
        const length = Buffer.alloc(4);
        length.writeUInt32BE(slot.length);
        payload = encodeCanonicalAuthorityPayload({
          body: Buffer.concat([
            body.subarray(0, -10),
            Buffer.of(1),
            length,
            slot,
            body.subarray(-5)
          ])
        });
      }
      const authority = {
        kind: 'snapshot',
        storeVersion: { value: recorded ? 'captured-version' : 'baseline' },
        payload,
        objectGeneration: 7n,
        authorityOwnerGeneration: 11n,
        ownerId: 'source-owner',
        ownerLeaseGeneration: 3n,
        allocation: {
          state: 'active',
          objectKind: 'actor',
          stableType: 'Player',
          descriptor: { meshName: 'mesh', rid: 'source' },
          descriptorLifecycleGeneration: 5n,
          capacity: { actors: 1, spots: 0 }
        }
      };
      const events = [];
      let aggregateRequest;
      const runtime = new ZLinkHostServiceRelocationRuntime({
        locationStore: () => ({
          async readAuthority() {
            return authority;
          },
          async prepareAggregate(request) {
            events.push('aggregate');
            aggregateRequest = request;
            return {
              kind: 'prepared',
              fence: { aggregateId: request.aggregateId, aggregateGeneration: 1n }
            };
          },
          async commitAggregate() {
            events.push('newOwner');
          }
        }),
        spotManager: () => ({ entrySpotIdForMesh: () => 'target-entry' })
      });
      if (changed === 'owner') coordinator.ownerId = 'other-owner';
      if (changed === 'lease') coordinator.leaseGeneration = 4n;
      if (changed === 'baseline') coordinator.expectedAuthorityStoreVersion = 'other-version';
      const prepare = {
        coordinator,
        target: {
          ownerId: 'target-owner',
          ownerLeaseGeneration: 13n,
          nodeRid: 'target',
          nodeGeneration: 17n
        },
        targetAttemptGeneration: 1n,
        payloadChecksumCrc32c: 0
      };
      const envelope = {
        aggregateId,
        aggregateGeneration: 1n,
        participants: [
          {
            participantId: 1n,
            key,
            objectKind: 'actor',
            stableType: 'Player',
            objectGeneration: 7n,
            authorityOwnerGeneration: 11n,
            applicationState: new Uint8Array(),
            boundSessionState: new Uint8Array(),
            queuedMessages: [],
            timers: []
          }
        ],
        memberships: []
      };
      if (changed === 'none') {
        await runtime.reserveTargetForPrepare('mesh', prepare, envelope);
        assert.deepEqual(events, ['aggregate']);
        assert.equal(
          aggregateRequest.participants[0].expectedStoreVersion.value,
          authority.storeVersion.value
        );
        const publication = new ServiceRelocationAuthorityPayloadCodec().read(
          aggregateRequest.participants[0].authorityPayload
        );
        assert.equal(publication.coordinatorExpectedStoreVersion, 'baseline');
      } else {
        await assert.rejects(
          runtime.reserveTargetForPrepare('mesh', prepare, envelope),
          /coordinator.*fence|authority changed/
        );
        assert.deepEqual(
          events,
          [],
          'rejected PREPARE must not prepare an aggregate or transfer ownership'
        );
      }
    });
  }
}
