const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist/contracts');
const {
  ZLinkInstanceActivationAuthority
} = require('../../packages/framework/dist/runtime/host/instance-activation-authority');
const {
  encodeInstanceActivationRecoveryEnvelope
} = require('../../packages/framework/dist/runtime/foundation/service-instance-activation-recovery-codec');
const {
  encodeApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  encodeServiceInstanceAuthorityPayload
} = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');

for (const state of ['reserved', 'active']) {
  for (const field of [
    'mesh',
    'type',
    'descriptor',
    'deadline',
    'operation',
    'metadata',
    'metadataBytes',
    'match'
  ]) {
    test(`${state}: stored ZLIA ${field} preserves Store`, async () => {
      const target = {
        targetSpotId: 'spot',
        stableType: 'room',
        targetNodeRid: 'target',
        targetNodeGeneration: 3n,
        targetMeshName: 'mesh',
        descriptorVersion: '1'
      };
      const payload = encodeApplicationPayload({
        packetName: 'Notice',
        contentType: 'application/json',
        payload: Buffer.from('{}')
      });
      const record = {
        kind: 'instanceSpot',
        activation: 'missing',
        target,
        sourceNodeRid: 'source',
        sourceNodeGeneration: 5n,
        operationKind: 'send',
        operation: { high: 1n, low: 2n },
        deadlineUnixMs: BigInt(Date.now() + 5000)
      };
      const stored = {
        ...record,
        target: { ...target },
        targetMeshName: 'mesh',
        applicationPayloadFrame: payload
      };
      if (field === 'mesh') stored.targetMeshName = 'other';
      if (field === 'type') stored.target.stableType = 'other';
      if (field === 'descriptor') stored.target.descriptorVersion = '2';
      if (field === 'deadline') stored.deadlineUnixMs += 1n;
      if (field === 'operation') {
        stored.operation = { high: 1n, low: 3n };
        stored.deadlineUnixMs += 1n;
      }
      if (field.startsWith('metadata'))
        stored.metadataFrame =
          require('../../packages/framework/dist/runtime/foundation/service-metadata-codec').encodeServiceMetadataFrame(
            new Map([['key', 'value']])
          );
      const root = encodeInstanceActivationRecoveryEnvelope(stored);
      const snapshot = {
        kind: 'snapshot',
        storeVersion: { value: 'v1' },
        objectGeneration: 1n,
        authorityOwnerGeneration: 1n,
        ownerId: 'owner',
        ownerLeaseGeneration: 1n,
        allocation: {
          state,
          objectKind: 'instance_spot',
          stableType: 'room',
          descriptor: { meshName: 'mesh', rid: 'target' },
          descriptorLifecycleGeneration: 3n
        },
        ...(state === 'reserved' ? { pendingCreation: { requestContentReference: 'root' } } : {}),
        payload: encodeServiceInstanceAuthorityPayload({
          state: state === 'active' ? 'ready' : 'coldActivating',
          stableType: 'room',
          spotId: 'spot',
          ownerId: 'owner',
          ownerLeaseGeneration: 1n,
          ownerMeshName: 'mesh',
          ownerNodeRid: 'target',
          ownerNodeGeneration: 3n,
          ...(state === 'active'
            ? {
                activationRecovery: {
                  reference: 'root',
                  sha256: require('node:crypto').createHash('sha256').update(root).digest(),
                  encodedSize: root.length,
                  inboxSequence: 1n,
                  replayCursor: 0n
                }
              }
            : {})
        })
      };
      let writes = 0;
      const before = structuredClone(snapshot);
      const authority = new ZLinkInstanceActivationAuthority({
        store: {
          async readAuthority() {
            return snapshot;
          },
          async releaseEndedReservation() {
            writes++;
            return true;
          },
          async reserve() {
            writes++;
            throw new Error('unexpected reserve');
          }
        },
        relocationStore: {
          async read() {
            return { kind: 'found', bytes: root };
          },
          async put() {
            writes++;
            throw new Error('unexpected put');
          }
        },
        meshName: 'mesh',
        owner: () => ({ ownerId: 'owner', leaseGeneration: 1n })
      });
      const runtime = new ServiceStatefulRuntime({ setServiceIngress() {} }, 'target', 3n);
      runtime.registerAsyncInstanceActivationAuthority(authority);
      try {
        const metadata =
          field === 'metadataBytes'
            ? require('../../packages/framework/dist/runtime/foundation/service-metadata-codec').encodeServiceMetadataFrame(
                new Map([['key', 'other']])
              )
            : undefined;
        if (field === 'match' || field === 'operation') {
          const result = await authority.read(target, undefined, {
            ...record,
            targetMeshName: 'mesh',
            applicationPayloadFrame: payload
          });
          assert.equal(result.kind, state === 'reserved' ? 'creating' : 'ready');
        } else
          await assert.rejects(
            runtime.runMissingInstanceActivation(record, payload, metadata),
            (error) =>
              error.kind === ZLinkFrameworkErrorKind.ProtocolError &&
              /stored activation/.test(error.message)
          );
        assert.equal(writes, 0);
        assert.deepEqual(structuredClone(snapshot), before);
      } finally {
        runtime.close();
      }
    });
  }
}
