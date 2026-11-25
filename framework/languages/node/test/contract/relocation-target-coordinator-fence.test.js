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
  decodeCanonicalAuthorityPayload,
  encodeCanonicalAuthorityPayload
} = require('../../packages/framework/dist/runtime/actors/actor-authority-payload-codec');

for (const phase of ['steady', 'captured', 'legacy captured']) {
  const changes = ['none', 'owner', 'lease', 'node', 'node generation', 'baseline'];
  if (phase === 'captured') {
    changes.push(
      'recorded owner',
      'recorded lease',
      'recorded node',
      'recorded node generation',
      'recorded baseline'
    );
  }
  for (const changed of changes) {
    test(`target PREPARE compares the coordinator fence (${phase}, ${changed})`, async () => {
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
      if (phase === 'legacy captured') {
        // Legacy .NET Capture writes the routing IDs as raw bytes.
        const slot = Buffer.from(
          'AAAAAAAAAAAAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAAAB3BlbmRpbmcAAAAABnNvdXJjZQAAAAAAAAAFDHNvdXJjZS1vd25lcgAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAAAADHNvdXJjZS1vd25lcgAAAAAAAAADBnNvdXJjZQAAAAAAAAAFCGJhc2VsaW5lAgAAAAAAAAAAAA==',
          'base64'
        );
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
      const source = {
        ownerId: coordinator.ownerId,
        ownerLeaseGeneration: coordinator.leaseGeneration,
        nodeRid: Buffer.from(coordinator.nodeRid).toString('hex'),
        nodeGeneration: coordinator.nodeGeneration
      };
      const recordedCoordinator = { ...source, expectedStoreVersion: 'baseline' };
      if (changed === 'recorded owner') recordedCoordinator.ownerId = 'other-owner';
      if (changed === 'recorded lease') recordedCoordinator.ownerLeaseGeneration = 4n;
      if (changed === 'recorded node') recordedCoordinator.nodeRid = 'ff';
      if (changed === 'recorded node generation') recordedCoordinator.nodeGeneration = 6n;
      if (changed === 'recorded baseline')
        recordedCoordinator.expectedStoreVersion = 'other-version';
      const authority = {
        kind: 'snapshot',
        storeVersion: { value: phase === 'steady' ? 'baseline' : 'captured-version' },
        payload,
        ...(phase === 'captured' && {
          relocation: {
            relocationId: aggregateId,
            phase: 'captured',
            source,
            target: null,
            coordinator: recordedCoordinator,
            sourceInventorySha256: '0'.repeat(64),
            applicationVersion: 0n
          }
        }),
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
      if (changed === 'node') coordinator.nodeRid = 'other-node';
      if (changed === 'node generation') coordinator.nodeGeneration = 6n;
      if (changed === 'baseline') coordinator.expectedAuthorityStoreVersion = 'other-version';
      const prepare = {
        coordinator,
        sourceNodeRid: 'source',
        sourceNodeGeneration: 5n,
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
      const entries = [
        {
          authorityKey: key,
          ownerTransition: 'newOwner',
          authorityPayload: payload,
          membershipMutation: Buffer.from('[]')
        }
      ];
      const digest = '0'.repeat(64);
      if (changed === 'none') {
        await runtime.reserveTargetForPrepare('mesh', prepare, envelope, entries, digest);
        assert.deepEqual(events, ['aggregate']);
        assert.equal(
          aggregateRequest.participants[0].expectedStoreVersion.value,
          authority.storeVersion.value
        );
        assert.deepEqual(aggregateRequest.participants[0].relocation.coordinator, {
          ...source,
          expectedStoreVersion: 'baseline'
        });
      } else {
        await assert.rejects(
          runtime.reserveTargetForPrepare('mesh', prepare, envelope, entries, digest),
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
