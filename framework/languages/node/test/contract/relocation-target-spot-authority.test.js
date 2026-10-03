const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const { encodeAuthorityKey } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

// The target materializes a relocated Spot before the target-only CAS, so it
// can only predict the authority owner generation. A newOwner participant
// receives its generation from the Store's owner-generation block, which may
// differ from that prediction. After the commit the native Spot must carry
// the committed authority, as the Actor branch does with restoreActorAuthority;
// otherwise every exact owner fence sent to the target is rejected as SpotMoving.
for (const objectKind of ['instance_spot', 'user_spot']) {
  test(`relocated ${objectKind} publishes the committed authority onto its native Spot`, async () => {
    const spotId = 'order-runner-relocation-1';
    const native = new Map();
    const node = {
      restoreSpotAuthority(id, kind, stableType, objectGeneration, authorityOwnerGeneration) {
        native.set(id, { kind, stableType, objectGeneration, authorityOwnerGeneration });
      },
      rememberSpotRoute() {}
    };
    const activation = { meshName: 'mesh', spotId };
    const registration = { implementation: class Order {}, relocation: { kind: 'recreate' } };
    const runtime = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([
          [
            'mesh',
            objectKind === 'instance_spot'
              ? { instanceSpotFactoryRegistrations: { Order: registration } }
              : { spotFactoryRegistrations: { Order: registration } }
          ]
        ])
      },
      meshNode: () => node,
      spotManager: () => ({
        // The production manager creates the native Spot from these values
        // through createNativeSpot -> restoreSpotAuthority.
        async prepareRelocationSpot(_mesh, kind, stableType, _type, id, objectGeneration, authorityOwnerGeneration) {
          node.restoreSpotAuthority(id, kind, stableType, objectGeneration, authorityOwnerGeneration);
          return activation;
        },
        async publishRelocationSpot() {}
      })
    });
    const participant = {
      participantId: 1n,
      key: encodeAuthorityKey(objectKind, spotId).value,
      objectKind,
      stableType: 'Order',
      objectGeneration: 13n,
      authorityOwnerGeneration: 13n,
      applicationState: new Uint8Array(),
      boundSessionState: new Uint8Array(),
      queuedMessages: [],
      timers: []
    };
    const stage = await runtime.materializeTargetEnvelope('mesh', {
      aggregateId: 'aggregate-1',
      aggregateGeneration: 1n,
      participants: [participant],
      memberships: []
    });
    assert.equal(native.get(spotId).authorityOwnerGeneration, 14n);

    const committed = {
      objectGeneration: 13n,
      authorityOwnerGeneration: 18n,
      ownerId: 'target-owner',
      ownerLeaseGeneration: 1n,
      storeVersion: { value: '171' },
      allocation: {
        objectKind,
        stableType: 'Order',
        descriptor: { rid: 'target-node' },
        descriptorLifecycleGeneration: 7n
      }
    };
    await stage.owner.publish(stage.staging, committed);

    assert.deepEqual(native.get(spotId), {
      kind: objectKind,
      stableType: 'Order',
      objectGeneration: 13n,
      authorityOwnerGeneration: 18n
    });
  });
}
