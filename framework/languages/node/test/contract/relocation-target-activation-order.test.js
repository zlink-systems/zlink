const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');

function target() {
  const runtime = new ZLinkHostServiceRelocationRuntime({});
  const events = [];
  runtime.commitTargetReservation = async () => {
    events.push('committed');
    return { storeVersion: { value: '1' } };
  };
  runtime.finalizeActorJoinProfiles = async () => events.push('completion');
  runtime.clearTargetRelocationPublication = async (_stage, authority) => {
    events.push('normalized');
    return authority;
  };
  runtime.publishSessionRoutes = async () => events.push('command44');
  runtime.targetReplyRelayCoordinator = () => ({});
  const stage = {
    phase: 'ready',
    releaseActivation() {},
    releaseJoinAdmission() {},
    offer: { reservation: {}, prepareFingerprint: 'same' },
    staging: { hidden: new Map(), envelope: { participants: [] } },
    owner: {
      async normalize() {},
      async publish() {},
      async openAdmission() {
        events.push('handler');
      }
    }
  };
  return { runtime, stage, events };
}

async function finalize(runtime, stage) {
  try {
    await runtime.finalizeTargetStage('mesh', 'move-1', stage);
  } catch (error) {
    throw error.cause ?? error;
  }
}

test('the previous target move submits command 44 before its first handler starts the next Join', async () => {
  const { runtime, stage, events } = target();
  stage.owner.openAdmission = async () => {
    events.push('handler');
    assert.equal(stage.phase, 'open');
    assert.ok(events.includes('command44'), 'previous Session route must precede the next Join');
    events.push('next-join');
  };
  await finalize(runtime, stage);
  assert.ok(events.indexOf('committed') < events.indexOf('handler'));
});

test('terminal cleanup follows Join completion and cannot overwrite the next root', async () => {
  const { runtime, stage, events } = target();
  const {
    encodeAuthorityKey
  } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
  const key = encodeAuthorityKey('actor', 'next-root');
  stage.staging.primaryAuthorityKey = key;
  let root = 'first-root';
  let version = '1';
  let attempts = 0;
  runtime.commitTargetReservation = async () => ({
    storeVersion: { value: version },
    payload: Buffer.from(root),
    allocation: { objectKind: 'actor' }
  });
  runtime.options.locationStore = () => ({
    async compareExchangeAuthority(actual, expected) {
      attempts++;
      events.push('normalized');
      assert.equal(actual.value, key.value);
      assert.equal(expected.value, '1', 'cleanup must retain its original authority fence');
      assert.equal(version, '2');
      return { kind: 'conflict' };
    }
  });
  delete runtime.clearTargetRelocationPublication;
  runtime.codec.read = () => ({ reference: 'first-root' });
  runtime.codec.clear = () => Buffer.alloc(0);
  runtime.finalizeActorJoinProfiles = async () => {
    assert.equal(attempts, 0, 'terminal cleanup must follow OnJoinCompleted');
    root = 'second-root';
    version = '2';
    events.push('completion');
  };
  await finalize(runtime, stage);
  await finalize(runtime, stage);
  assert.equal(root, 'second-root');
  assert.equal(attempts, 1);
  assert.ok(events.indexOf('handler') < events.indexOf('normalized'));
});

test('request terminal relay uses the activated authority before terminal cleanup', async () => {
  const { runtime, stage, events } = target();
  const packet = { index: 0, returnResponse: true, source: { replyRouteId: '1' } };
  const hidden = { actor: { context: { actorId: 'actor' } }, replayPackets: [packet] };
  const {
    encodeAuthorityKey
  } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
  const key = encodeAuthorityKey('actor', 'actor');
  stage.staging.hidden.set(key.value, hidden);
  stage.staging.envelope.participants.push({ key: key.value });
  const {
    RelocationCompletionStore
  } = require('../../packages/framework/dist/runtime/locations/relocation-completion-store');
  const relocationId = '11111111-1111-1111-1111-111111111111';
  const targetOwner = { ownerId: 'target-owner', ownerLeaseGeneration: 1n };
  let current = {
    kind: 'snapshot',
    ...targetOwner,
    storeVersion: { value: '1' },
    relocation: {
      relocationId,
      phase: 'prepared',
      sourceInventorySha256: '0'.repeat(64),
      completion: {
        acceptedRequests: 1n,
        completedResults: 0n,
        pendingDeliveries: 0n,
        reference: null,
        sha256: null
      }
    }
  };
  const store = {
    async readAuthority() {
      return current;
    },
    async compareExchangeAuthority(_key, expected, mutation) {
      assert.equal(expected.value, current.storeVersion.value);
      current = {
        ...current,
        relocation: mutation.relocation,
        storeVersion: { value: `${expected.value}:${mutation.relocation.phase}` }
      };
      return { ...current, kind: 'stored' };
    }
  };
  const blobs = new Map();
  const blobStore = {
    async put(reference, bytes, retention) {
      blobs.set(reference.value, Buffer.from(bytes));
      return { kind: 'stored', storeNow: new Date(0), expiresAt: new Date(retention) };
    },
    async read(reference) {
      const bytes = blobs.get(reference.value);
      return bytes === undefined
        ? { kind: 'missing', storeNow: new Date(0) }
        : { kind: 'found', bytes, storeNow: new Date(0), expiresAt: new Date(86400000) };
    }
  };
  stage.offer.prepare = { target: targetOwner };
  stage.staging.envelope.aggregateId = relocationId;
  stage.staging.primaryAuthorityKey = key;
  runtime.options.locationStore = () => store;
  runtime.options.relocationStore = blobStore;
  runtime.commitTargetReservation = async () => current;
  const completion = new RelocationCompletionStore(store, blobStore);
  runtime.clearTargetRelocationPublication = async () => {
    events.push('normalized');
    return { storeVersion: { value: 'normalized-v2' } };
  };
  runtime.targetReplyRelayCoordinator = (_mesh, _stage, authority) => ({
    expectedAuthorityStoreVersion: authority.storeVersion.value
  });
  const observed = [];
  runtime.relayTerminalReply = async (_mesh, _stage, coordinator) => {
    events.push('relay');
    observed.push(coordinator.expectedAuthorityStoreVersion);
    const entry = {
      objectKey: key.value,
      source: { ownerId: 'requester', leaseGeneration: '1', nodeRid: '61', nodeGeneration: '1' },
      operationId: '00000000000000010000000000000001',
      replyRouteId: '1',
      reply: '',
      delivery: 'pending'
    };
    await completion.append(key, current, entry);
    return completion.acknowledge(key, current, entry, 'terminalReceived');
  };
  stage.owner.normalize = async () => {
    await hidden.replayStart;
    await hidden.onReplayResult(packet, { index: 0, ok: true, response: 'reply' });
  };
  await finalize(runtime, stage);
  assert.deepEqual(observed, ['1:committed:activating:activated']);
  assert.ok(events.indexOf('handler') < events.indexOf('relay'));
  assert.ok(events.indexOf('relay') < events.indexOf('normalized'));
});

for (const primary of [false, true]) {
  for (const stored of [false, true]) {
    test(`Actor publication cleanup observes only successful CAS results (primary=${primary}, stored=${stored})`, async () => {
      const {
        encodeAuthorityKey
      } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
      const key = encodeAuthorityKey('actor', 'returning-actor');
      const observations = [];
      const current = {
        kind: 'snapshot',
        payload: new Uint8Array(),
        storeVersion: { value: 'before' },
        allocation: { objectKind: 'actor' }
      };
      const runtime = new ZLinkHostServiceRelocationRuntime({
        locationStore: () => ({
          async readAuthority() {
            return current;
          },
          async compareExchangeAuthority(actualKey, expected) {
            assert.equal(actualKey.value, key.value);
            assert.equal(expected.value, 'before');
            return {
              ...current,
              kind: stored ? 'stored' : 'conflict',
              storeVersion: { value: 'after' }
            };
          }
        }),
        observeActorAuthorityVersion: (actorId, version) => observations.push([actorId, version])
      });
      runtime.codec.read = () => ({ reference: {} });
      runtime.codec.clear = () => new Uint8Array();
      const envelope = { participants: [{ key: key.value, objectKind: 'actor' }] };
      const operation = primary
        ? runtime.clearTargetRelocationPublication(
            { staging: { envelope, primaryAuthorityKey: key } },
            current
          )
        : runtime.releaseParticipantAuthorities(envelope, 'different-primary');
      if (stored) await operation;
      else await assert.rejects(operation, /CAS failed|recovery pointer release failed/);
      assert.deepEqual(observations, stored ? [['returning-actor', 'after']] : []);
    });
  }
}
