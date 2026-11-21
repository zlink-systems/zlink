const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkFrameworkRuntimeHost } = require('../../packages/framework/dist/runtime/host');
const {
  ZLinkRequestSourceResolver
} = require('../../packages/framework/dist/runtime/locations/request-source-resolver');
const {
  ZLinkActorNativeJoinCoordinator
} = require('../../packages/framework/dist/runtime/actors/actor-remote-joiner');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const {
  ZLinkFrameworkRuntimeState,
  ZLinkSpotKind,
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/internal');

function descriptor(rid, changes = {}) {
  return {
    meshName: 'play',
    rid,
    lifecycleGeneration: 7n,
    descriptorRevision: 2n,
    ownerId: rid,
    leaseGeneration: 3n,
    objectRole: 'server',
    entrySpotId: `entry-${rid}`,
    state: ZLinkFrameworkRuntimeState.Serving,
    placementWeight: 1,
    populationCapacity: { actors: { active: 0, reserved: 0, limit: 2 } },
    objectCapabilities: [{ objectKind: 'actor', stableType: 'player', entryAccepted: true }],
    ...changes
  };
}
async function observePlacement(rows) {
  const requestSources = new ZLinkRequestSourceResolver({
    listLiveMeshNodes: async () => {
      assert.fail('placement must use the discovery observation');
    },
    remainingOwnerTokenLeaseMs: async ({ ownerId }) => (ownerId === 'expired' ? 0 : 30_000)
  });
  await requestSources.observeLiveDescriptors(
    'play',
    rows.filter((row) => row.meshName === 'play')
  );
  return requestSources;
}
for (const [name, changes] of Object.entries({
  'non-serving': { state: ZLinkFrameworkRuntimeState.Draining },
  'expired lease': { ownerId: 'expired' },
  'wrong role': { objectRole: 'client' },
  'wrong Mesh': { meshName: 'other' },
  'wrong stable type': {
    objectCapabilities: [{ objectKind: 'actor', stableType: 'other', entryAccepted: true }]
  },
  'wrong object kind': {
    objectCapabilities: [{ objectKind: 'user_spot', stableType: 'player', entryAccepted: true }]
  },
  'entryAccepted false': {
    objectCapabilities: [{ objectKind: 'actor', stableType: 'player', entryAccepted: false }]
  },
  'full capacity': { populationCapacity: { actors: { active: 1, reserved: 1, limit: 2 } } },
  'zero weight': { placementWeight: 0 },
  'missing Entry Spot': { entrySpotId: undefined }
})) {
  test(`Entry placement excludes ${name}`, async () => {
    const resolver = await observePlacement([descriptor('source', changes), descriptor('remote')]);
    assert.equal(
      (await resolver.selectPlacement('play', 'actor', 'player', undefined)).rid,
      'remote'
    );
  });
}
test('Entry placement applies weight without preferring the current node', async () => {
  // Force each end of the random byte range; the existing selector uses 64-bit-safe weights.
  const crypto = require('node:crypto');
  const original = crypto.randomBytes;
  try {
    const resolver = await observePlacement([
      descriptor('source'),
      descriptor('remote', { placementWeight: 2 })
    ]);
    const node = {
      status: () => ({ routingId: 'source', lifecycleGeneration: 7n }),
      isPeerRouteReady: () => true
    };
    crypto.randomBytes = (n) => Buffer.alloc(n, 0);
    assert.equal((await resolver.selectPlacement('play', 'actor', 'player', node)).rid, 'source');
    crypto.randomBytes = (n) => Buffer.alloc(n, 1);
    assert.equal((await resolver.selectPlacement('play', 'actor', 'player', node)).rid, 'remote');
  } finally {
    crypto.randomBytes = original;
  }
});
test('Entry placement permits unlimited capacity and has no fallback candidate', async () => {
  const unlimited = await observePlacement([
    descriptor('source', {
      populationCapacity: { actors: { active: 99, reserved: 9, limit: 0 } }
    })
  ]);
  assert.equal(
    (await unlimited.selectPlacement('play', 'actor', 'player', undefined)).rid,
    'source'
  );
  const zeroWeight = await observePlacement([descriptor('source', { placementWeight: 0 })]);
  assert.equal(await zeroWeight.selectPlacement('play', 'actor', 'player', undefined), undefined);
});
function nativeAdmission() {
  const calls = [];
  const ref = { actorId: 'alice', nodeRid: 'source', generation: 1n };
  return {
    calls,
    ref,
    node: {
      status: () => ({ routingId: 'source', lifecycleGeneration: 7n }),
      joinActorEntrySpot: (_actor, target, payload) => {
        calls.push({ target, payload: JSON.parse(payload.data().toString()) });
        return { high: 1n, low: 1n };
      }
    },
    completions: {
      submit: async (callback) => {
        callback();
        return {
          terminalResult: 0,
          failureErrno: 0,
          parts: [],
          kindData: { kind: 'actorJoinCompletion', actor: ref, joinResult: 1 }
        };
      }
    }
  };
}
function sourceState(ref) {
  const state = new ZLinkActorRuntimeState('alice');
  state.rememberMeshName('play');
  state.getOrStartCreation('player', false, async () => ({ status: 'created', actor: {} }));
  state.setNativeActorRef(ref);
  state.setJoinedSpot('room', undefined, 5n, 2n);
  return state;
}
const sourceActor = { context: { actorId: 'alice', meshName: 'play' } };
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');

test('Entry Join submits the selected remote route and preserves rejected source membership', async () => {
  const backend = nativeAdmission();
  const state = sourceState(backend.ref);
  const coordinator = new ZLinkActorNativeJoinCoordinator({
    node: backend.node,
    completionTableProvider: () => backend.completions,
    entrySpotTargetSelector: async () => ({
      routerChannelId: 'play',
      targetNodeRid: 'remote',
      spotId: 'entry-remote',
      spotKind: ZLinkSpotKind.Entry,
      targetSpotGeneration: 7n,
      targetNodeGeneration: 7n,
      authorityOwnerGeneration: 7n,
      ownerLeaseGeneration: 3n
    })
  });
  const request = ZLinkBufferMessage.from('{}');
  try {
    const result = await coordinator.joinEntrySpot(
      sourceActor,
      state,
      'stale-entry-node',
      request,
      1000
    );
    assert.equal(result.accepted, false);
    assert.equal(backend.calls[0].target.targetNodeRid, 'remote');
    assert.equal(backend.calls[0].target.spot.spotId, 'entry-remote');
    assert.equal(backend.calls[0].target.targetNodeGeneration, 7n);
    assert.equal(backend.calls[0].target.ownerLeaseGeneration, 3n);
    assert.equal(backend.calls[0].payload.spotId, 'entry-remote');
    assert.equal(state.spotId, 'room');
  } finally {
    request.close();
  }
});
test('Entry Join with no eligible candidate returns Unavailable without local fallback', async () => {
  const backend = nativeAdmission();
  const state = sourceState(backend.ref);
  const coordinator = new ZLinkActorNativeJoinCoordinator({
    node: backend.node,
    completionTableProvider: () => backend.completions,
    entrySpotTargetSelector: async () => undefined
  });
  const request = ZLinkBufferMessage.from('{}');
  try {
    await assert.rejects(
      coordinator.joinEntrySpot(sourceActor, state, 'source', request, 1000),
      (e) => e.kind === ZLinkFrameworkErrorKind.Unavailable
    );
    assert.equal(backend.calls.length, 0);
    assert.equal(state.spotId, 'room');
  } finally {
    request.close();
  }
});

test('host wires Actor Mesh/type selection to the chosen Entry descriptor', async (t) => {
  const backend = nativeAdmission();
  const requestSources = await observePlacement([descriptor('remote')]);
  const selection = t.mock.method(requestSources, 'selectPlacement');
  const routeNode = { ...backend.node, isPeerRouteReady: () => true };
  const host = Object.assign(Object.create(ZLinkFrameworkRuntimeHost.prototype), {
    options: { registration: {} },
    locationOwner: {},
    meshRouters: { primaryMeshName: () => 'wrong-primary-mesh' },
    createLocationSpotRouteResolver: () => undefined,
    spotNodeRuntime: {
      primaryMeshNode: backend.node,
      primaryMeshCompletions: backend.completions,
      meshNode: (meshName) => (meshName === 'play' ? routeNode : undefined),
      locationAutoConnect: { requestSources }
    }
  });
  const coordinator = host.actorRuntimeOptionsFactory().createActorManagerOptions().joinCoordinator;
  const controller = new AbortController();
  const request = ZLinkBufferMessage.from('{}');
  try {
    await coordinator.joinEntrySpot(
      sourceActor,
      sourceState(backend.ref),
      undefined,
      request,
      1000,
      controller.signal
    );
    assert.deepEqual(
      selection.mock.calls.map(({ arguments: args }) => args),
      [['play', 'actor', 'player', routeNode, controller.signal]]
    );
    assert.equal(backend.calls[0].target.targetNodeRid, 'remote');
    assert.equal(backend.calls[0].target.spot.spotId, 'entry-remote');
    assert.equal(backend.calls[0].target.ownerLeaseGeneration, 3n);
    assert.equal(backend.calls[0].payload.spotId, 'entry-remote');
  } finally {
    request.close();
  }
});

for (const entry of [true, false]) {
  test(`canonical ${entry ? 'Entry' : 'User'} admission remains the only reply decision before Actor restore`, async () => {
    const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');
    const {
      ZLinkFormalRemoteActorAdmissionRegistry
    } = require('../../packages/framework/dist/runtime/spots/formal-remote-actor-admission-registry');
    const {
      ZLinkBufferMessage
    } = require('../../packages/framework/dist/runtime/backend/runtime-message');
    const admissions = new ZLinkFormalRemoteActorAdmissionRegistry();
    const replies = [];
    let callbacks = 0;
    const request = ZLinkBufferMessage.from('{}');
    const activation = entry
      ? undefined
      : {
          domain: { kind: 'user' },
          serial: {
            execute: (callback) => callback(),
            executeLifecycleOperation: (callback) => callback()
          },
          spot: {
            onActorJoin: async () => {
              callbacks++;
              return { accepted: false };
            }
          }
        };
    const manager = Object.assign(Object.create(DefaultZLinkSpotManager.prototype), {
      options: {
        entrySpotIdProvider: () => 'entry',
        dispatchEntryActorJoin: () => {
          callbacks++;
        },
        canonicalActorJoinResolver: async () => ({ actorType: 'player' })
      },
      activations: { resolve: () => activation },
      formalRemoteActorAdmissions: admissions,
      formalRemoteTransfers: new Map()
    });
    const record = {
      contentType: 'application/json',
      parts: [request],
      kindData: {
        kind: 'actorControl',
        currentActor: { actorId: 'alice', nodeRid: 'source', generation: 1n },
        currentSpotGeneration: 7n,
        currentMembershipEpoch: 2n,
        canonicalActorJoin: { handoffId: 'canonical-entry', actorNodeRid: 'source' }
      },
      replyActorJoin: (result) => {
        replies.push(result);
        return 0;
      }
    };
    try {
      await manager.dispatchMeshActorJoin('play', { spotId: entry ? 'entry' : 'room' }, record);
      assert.deepEqual(replies, [entry ? 0 : 1]);
      assert.equal(callbacks, entry ? 0 : 1);
    } finally {
      admissions.abort('canonical-entry');
      request.close();
    }
  });
}
