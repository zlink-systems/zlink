const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  encodeAuthorityAggregate
} = require('../../packages/framework/dist/runtime/locations/authority-relocation-record');

for (const outcome of ['success', 'failure']) {
  test(`terminal progress waits for admission and ${outcome} request delivery`, async () => {
    const fence = {
      nodeRid: 'target',
      nodeGeneration: 6n,
      ownerId: 'target-owner',
      ownerLeaseGeneration: 14n
    };
    const key = encodeAuthorityKey('actor', 'terminal');
    const events = [];
    let finish;
    const delivery = new Promise((resolve, reject) => {
      finish = outcome === 'success' ? resolve : reject;
    });
    let current = {
      kind: 'snapshot',
      ownerId: fence.ownerId,
      ownerLeaseGeneration: fence.ownerLeaseGeneration,
      storeVersion: { value: 'v1' },
      payload: Buffer.from('opaque'),
      relocation: {
        relocationId: '11111111-1111-1111-1111-111111111111',
        phase: 'prepared',
        source: { ...fence, nodeRid: 'source' },
        target: { ...fence, attemptGeneration: 1n },
        coordinator: { ...fence, expectedStoreVersion: 'source-v1' },
        sourceInventorySha256: '0'.repeat(64),
        applicationVersion: 1n,
        completion: {
          acceptedRequests: 1n,
          completedResults: 0n,
          pendingDeliveries: 0n,
          reference: null,
          sha256: null
        }
      },
      allocation: { objectKind: 'actor' }
    };
    const blobs = new Map();
    const runtimeOptions = {
      relocationStore: {
        async put(reference, bytes, retention) {
          blobs.set(reference.value, Buffer.from(bytes));
          return { kind: 'stored', storeNow: new Date(0), expiresAt: new Date(retention) };
        },
        async read(reference) {
          return blobs.has(reference.value)
            ? {
                kind: 'found',
                bytes: blobs.get(reference.value),
                storeNow: new Date(0),
                expiresAt: new Date(86400000)
              }
            : { kind: 'missing', storeNow: new Date(0) };
        }
      },
      locationStore: () => ({
        async readAuthority() {
          return current;
        },
        async compareExchangeAuthority(_key, expected, mutation) {
          assert.equal(expected.value, current.storeVersion.value);
          current = {
            ...current,
            relocation: mutation.relocation ?? undefined,
            storeVersion: { value: `${expected.value}:cas` }
          };
          const json = encodeAuthorityAggregate(undefined, current.relocation);
          events.push(json?.relocation?.phase ?? 'clear');
          assert.equal(json?.relocation?.aggregateGeneration, undefined);
          return { ...current, kind: 'stored' };
        }
      }),
      spotManager: () => undefined,
      actorTransfer: {}
    };
    const runtime = new ZLinkHostServiceRelocationRuntime(runtimeOptions);
    runtime.commitTargetReservation = async () => current;
    const stage = {
      phase: 'ready',
      lane: Promise.resolve(),
      cutoverReceived: true,
      boundaryRelay: [],
      offer: { prepareFingerprint: 'terminal', prepare: { target: fence } },
      staging: {
        primaryAuthorityKey: key,
        envelope: {
          aggregateId: current.relocation.relocationId,
          participants: [{ key: key.value }],
          memberships: []
        },
        hidden: new Map()
      },
      owner: {
        async publish() {
          events.push('publish');
        },
        async normalize() {
          await delivery;
          const {
            RelocationCompletionStore
          } = require('../../packages/framework/dist/runtime/locations/relocation-completion-store');
          const completion = new RelocationCompletionStore(
            runtimeOptions.locationStore(),
            runtimeOptions.relocationStore
          );
          const entry = {
            objectKey: key.value,
            source: {
              ownerId: 'requester',
              leaseGeneration: '1',
              nodeRid: '61',
              nodeGeneration: '1'
            },
            operationId: '00000000000000010000000000000001',
            replyRouteId: '1',
            reply: '',
            delivery: 'pending'
          };
          await completion.append(key, current, entry);
          await completion.acknowledge(key, current, entry, 'terminalReceived');
          events.push('delivered');
        },
        async openAdmission() {
          events.push('open');
        }
      },
      releaseActivation() {},
      releaseJoinAdmission() {}
    };
    const finalized = runtime.finalizeTargetStage('mesh', 'terminal', stage);
    const settled = Promise.allSettled([finalized]);
    await turn();
    assert.deepEqual(events, ['committed', 'activating', 'publish', 'activated', 'open']);
    finish(outcome === 'failure' ? new Error('delivery failed') : undefined);
    const [result] = await settled;
    if (outcome === 'success') {
      assert.equal(result.status, 'fulfilled');
      assert.deepEqual(events.slice(-4), ['delivered', 'cleaning', 'completed', 'clear']);
    } else {
      assert.equal(result.status, 'rejected');
      assert.equal(current.relocation.phase, 'activated');
      assert.equal(events.includes('clear'), false);
    }
  });
}

for (const phase of ['preparing', 'captured', 'prepared']) {
  test(`source cleanup records Aborted before removing ${phase} progress`, async () => {
    const fence = {
      nodeRid: 'source',
      nodeGeneration: 2n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 3n
    };
    const key = encodeAuthorityKey('actor', 'aborted');
    const phases = [];
    let current = {
      kind: 'snapshot',
      storeVersion: { value: 'prepared' },
      payload: Buffer.from('opaque'),
      relocation: {
        relocationId: '11111111-1111-1111-1111-111111111111',
        phase,
        source: fence,
        target:
          phase === 'prepared' ? { ...fence, nodeRid: 'target', attemptGeneration: 1n } : null,
        coordinator: { ...fence, expectedStoreVersion: 'original' },
        sourceInventorySha256: '0'.repeat(64),
        applicationVersion: 1n
      }
    };
    const original = current.relocation;
    const runtime = new ZLinkHostServiceRelocationRuntime({
      locationStore: () => ({
        async readAuthority() {
          return current;
        },
        async compareExchangeAuthority(_key, expected, mutation) {
          assert.equal(expected.value, current.storeVersion.value);
          assert.deepEqual(mutation.payload, Buffer.from('opaque'));
          if (mutation.relocation != null)
            assert.deepEqual(mutation.relocation, { ...original, phase: 'aborted' });
          current = {
            ...current,
            relocation: mutation.relocation ?? undefined,
            storeVersion: { value: `${expected.value}:cas` }
          };
          phases.push(encodeAuthorityAggregate(undefined, current.relocation)?.relocation?.phase);
          return { ...current, kind: 'stored' };
        }
      })
    });
    await runtime.releaseParticipantAuthorities(
      { participants: [{ key: key.value }], memberships: [] },
      '',
      undefined,
      'aborted'
    );
    assert.deepEqual(phases, ['aborted', undefined]);
  });
}

test('Actor destruction joins only the same incarnation target terminal', async () => {
  const runtime = new ZLinkHostServiceRelocationRuntime({});
  const actor = { context: { actorId: 'same-id' } };
  const otherIncarnation = { context: { actorId: 'same-id' } };
  let finish;
  const terminal = new Promise((resolve) => {
    finish = resolve;
  });
  runtime.targetStages.set('pending', {
    staging: { hidden: new Map([['actor', { actor }]]) },
    finalize: terminal
  });
  await runtime.drainTargetActorRelocation(otherIncarnation);
  let released = false;
  const destruction = runtime.drainTargetActorRelocation(actor).then(() => {
    released = true;
  });
  await turn();
  assert.equal(released, false, 'authority release must wait for this incarnation lifecycle');
  finish();
  await destruction;
  assert.equal(released, true);
});
