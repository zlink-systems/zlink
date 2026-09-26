'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkPublicSpotManager
} = require('../../packages/framework/dist/runtime/spots/spot-manager-public');
const {
  ZLinkUserSpotCreationCoordinator
} = require('../../packages/framework/dist/runtime/host/user-spot-creation-coordinator');
const {
  ZLinkInMemoryAuthorityStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-authority-store');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  decodeServiceClosingSpotAuthority,
  decodeServiceReadySpotAuthority
} = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');
const {
  serviceRelocationAuthorityApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');

const fixture = JSON.parse(
  fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/conformance/spot-close-v1.json'),
    'utf8'
  )
);

const MESH = 'mesh';
const NODE_RID = 'node-a';
const OWNER_LIVENESS = `${MESH}:${NODE_RID}:1:owner-a:1`;

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

function errorKindName(error) {
  return Object.keys(framework.ZLinkFrameworkErrorKind).find(
    (name) => framework.ZLinkFrameworkErrorKind[name] === error?.kind
  );
}

function joiningActor(actorId) {
  return {
    actorId,
    context: {
      actorId,
      [framework.ZLINK_ACTOR_LIFECYCLE_SNAPSHOT]() {
        return {
          actorRef: { nodeRid: zlink.RoutingId.from(NODE_RID), actorId, generation: 1n },
          actorType: 'player',
          membershipEpoch: 1n
        };
      }
    }
  };
}

/**
 * One owner process: Location authority store, the User Spot authority
 * coordinator, the local Spot runtime and the public manager that joins them.
 */
async function createOwner(given) {
  const events = [];
  const diagnostics = [];
  const counters = { onClosing: 0, handler: 0, creationIntent: 0 };
  const live = new Set([OWNER_LIVENESS]);
  const base = new ZLinkInMemoryAuthorityStore(
    {
      isTargetLive(descriptor, lifecycle, token) {
        return live.has(
          `${descriptor.meshName}:${descriptor.rid}:${lifecycle}:${token.ownerId}:${token.leaseGeneration}`
        );
      }
    },
    () => new Date(100)
  );
  let releaseFailures = given.failOnce === 'authorityReleased' ? 1 : 0;
  const store = Object.create(base);
  store.compareExchangeAuthority = async (key, expected, mutation, signal) => {
    if (mutation.kind === 'delete' && releaseFailures > 0) {
      releaseFailures--;
      throw new Error('authority release failed once');
    }
    const result = await base.compareExchangeAuthority(key, expected, mutation, signal);
    if (result.kind === 'deleted') events.push('authorityReleased');
    return result;
  };
  const closingEntered = deferred();
  const finishClosing = deferred();
  if (given.holdOnClosing !== true) finishClosing.resolve();

  class RoomSpot {
    async onJoinedActor() {}

    async onActorJoin() {
      if (given.joinGate !== undefined) await given.joinGate.promise;
      return { accepted: true };
    }

    async onClosing() {
      counters.onClosing++;
      events.push('onClosing');
      closingEntered.resolve();
      await finishClosing.promise;
      if (given.onClosing === 'throws') throw new Error('OnClosing failed');
    }
  }

  let coordinator;
  const manager = new framework.DefaultZLinkSpotManager({
    spotFactories: [RoomSpot],
    closeErrorSink: {
      reportRuntimeTaskException: (name, error) => diagnostics.push({ name, error })
    },
    beginUserClosingAuthority: (meshName, spotId, objectGeneration, onCommitted) =>
      coordinator.beginOwnerClose(
        { spotId, objectGeneration, meshName, nodeRid: NODE_RID },
        onCommitted
      )
  });
  coordinator = new ZLinkUserSpotCreationCoordinator({
    store,
    target: async () => ({
      meshName: MESH,
      nodeRid: NODE_RID,
      nodeGeneration: 1n,
      owner: { ownerId: 'owner-a', leaseGeneration: 1n },
      isLocal: true
    }),
    pollIntervalMs: 1
  });
  const spots = new ZLinkPublicSpotManager({
    local: manager,
    coordinator,
    factories: new Map([[MESH, new Map([['room', { implementation: RoomSpot }]])]]),
    resolver: () => undefined,
    isLocalNode: () => true,
    defaultTimeoutMs: 1_000
  });
  const spotId = 'conformance-room';
  const key = encodeAuthorityKey('user_spot', spotId);
  let ref = { spotId, objectGeneration: 1n, meshName: MESH, nodeRid: NODE_RID };
  if (given.authority !== 'Missing') {
    await spots.getOrCreate(spotId, 'room').inMesh(MESH).submit();
    const created = await base.readAuthority(key);
    ref = { ...ref, objectGeneration: created.objectGeneration };
  }
  const authority = async () => {
    const current = await base.readAuthority(key);
    if (current.kind === 'missing') return 'Missing';
    const payload = serviceRelocationAuthorityApplicationPayload(current.payload);
    if (decodeServiceClosingSpotAuthority(payload) !== undefined) return 'Closing';
    if (decodeServiceReadySpotAuthority(payload) !== undefined) return 'Ready';
    return 'Other';
  };
  const admission = async () => {
    if (manager.isSpotClosing(MESH, spotId)) return 'sealed';
    const reply = await manager.executeOnSpot(RoomSpot, spotId, () => 'admitted');
    return reply === 'admitted' ? 'open' : 'sealed';
  };
  const join = async (actorId) => {
    const request = zlink.Message.from(JSON.stringify('join'));
    try {
      return await manager.admitActorJoin(spotId, joiningActor(actorId), request, () => undefined);
    } finally {
      request.close();
    }
  };
  const diagnosticKinds = () =>
    diagnostics.map(({ name }) => (/ onClosing$/.test(name) ? 'onClosingFailed' : name));
  return {
    RoomSpot,
    store: base,
    manager,
    spots,
    spotId,
    live,
    events,
    counters,
    closingEntered,
    finishClosing,
    authority,
    admission,
    join,
    diagnosticKinds,
    get ref() {
      return ref;
    }
  };
}

async function outcome(operation) {
  try {
    return await operation();
  } catch (error) {
    return errorKindName(error) ?? 'failure';
  }
}

const scenarios = {
  async 'close-absent-incarnation-is-false'(given) {
    const owner = await createOwner(given);
    assert.equal(await owner.authority(), given.authority);
    const result = await outcome(() => owner.spots.close(owner.ref));
    return { result, onClosingCalls: owner.counters.onClosing };
  },

  async 'close-other-generation-is-invalid-operation'(given) {
    const owner = await createOwner(given);
    assert.equal(given.closeGeneration, 'previous');
    const previous = { ...owner.ref, objectGeneration: owner.ref.objectGeneration - 1n };
    const result = await outcome(() => owner.spots.close(previous));
    return {
      result,
      authority: await owner.authority(),
      onClosingCalls: owner.counters.onClosing
    };
  },

  async 'close-with-membership-is-false-and-keeps-authority'(given) {
    const owner = await createOwner(given);
    for (let index = 0; index < given.members; index++) {
      assert.equal((await owner.join(`member-${index}`)).accepted, true);
    }
    const result = await outcome(() => owner.spots.close(owner.ref));
    return {
      result,
      authority: await owner.authority(),
      admission: await owner.admission(),
      onClosingCalls: owner.counters.onClosing
    };
  },

  async 'close-after-accepted-join-observes-its-membership'(given) {
    const joinGate = deferred();
    const owner = await createOwner({ ...given, joinGate });
    assert.equal(given.members, 0);
    const order = [];
    const joining = owner.join('late-member').then((result) => {
      order.push('joinCompleted');
      return result;
    });
    await new Promise((resolve) => setImmediate(resolve));
    const closing = outcome(() => owner.spots.close(owner.ref)).then((result) => {
      order.push('closeCompleted');
      return result;
    });
    await new Promise((resolve) => setImmediate(resolve));
    joinGate.resolve();
    assert.equal((await joining).accepted, true);
    const result = await closing;
    return {
      result,
      authority: await owner.authority(),
      admission: await owner.admission(),
      onClosingCalls: owner.counters.onClosing,
      order
    };
  },

  async 'failure-before-closing-commit-keeps-authority'(given) {
    const owner = await createOwner(given);
    assert.equal(given.closingCommit, 'ownerFenceMismatch');
    owner.live.delete(OWNER_LIVENESS);
    const result = await outcome(() => owner.spots.close(owner.ref));
    owner.live.add(OWNER_LIVENESS);
    return {
      result,
      authority: await owner.authority(),
      admission: await owner.admission(),
      onClosingCalls: owner.counters.onClosing
    };
  },

  async 'on-closing-failure-is-diagnostic-and-cleanup-continues'(given) {
    const owner = await createOwner(given);
    const result = await outcome(() => owner.spots.close(owner.ref));
    return {
      result,
      authority: await owner.authority(),
      onClosingCalls: owner.counters.onClosing,
      diagnostics: owner.diagnosticKinds()
    };
  },

};

test('Spot Close runtime satisfies every scenario of the shared spot-close fixture', async (t) => {
  assert.equal(fixture.fixture, 'zlink.framework.spot-close');
  assert.equal(fixture.version, 1);
  assert.equal(fixture.invariants.contextCloseReturnsValue, true);
  for (const scenario of fixture.scenarios) {
    await t.test(scenario.name, async () => {
      const run = scenarios[scenario.name];
      assert.ok(run, `unknown spot-close fixture scenario '${scenario.name}'`);
      const observed = await run(scenario.given);
      for (const [field, expected] of Object.entries(scenario.expect)) {
        assert.deepEqual(observed[field], expected, `${scenario.name}: ${field}`);
      }
    });
  }
});

test('context Close resolves after the target authority is released', async () => {
  const owner = await createOwner({ authority: 'Ready', holdOnClosing: true });
  let settled = false;
  let close;
  await owner.manager.executeOnSpot(owner.RoomSpot, owner.spotId, (spot) => {
    close = spot.context.close();
    return 'handlerReply';
  });
  close.finally(() => {
    settled = true;
  });
  await owner.closingEntered.promise;
  assert.equal(await owner.authority(), 'Closing');
  assert.equal(settled, false);
  owner.finishClosing.resolve();
  assert.equal(await close, true);
  assert.equal(await owner.authority(), 'Missing');
});
