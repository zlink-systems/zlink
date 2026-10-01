const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkSpotActorMembership } = require('../../packages/framework/dist/runtime/spots/spot-actor-membership');
const { ZLinkSpotSerialTurnExecutor } = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const framework = require('../../packages/framework/dist/internal');
const { DefaultZLinkActorContext } = require('../../packages/framework/dist/runtime/actors/actor-context');
const { ZLinkActorRuntimeState } = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const { runActorHandlerWithDeferredJoins } = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');

function fixture({ storeFailure = false, lifecycleFailure = false, entry = false, rejected = false, sameTarget = false, sourceLeave, admissionWaitForAbort = false, entrySpotId = 'node-a', destroyAfterJoined = false, postCommitBinder } = {}) {
  const events = [];
  let joinSignal;
  const completions = [];
  const storeMemberships = [];
  const shutdown = new AbortController();
  const actorRef = { actorId: 'alice', nodeRid: 'node-a', generation: 1n };
  let location = { actor: actorRef, spotId: 'node-a', spotGeneration: 1n, membershipEpoch: 1n };
  const state = new ZLinkActorRuntimeState('alice');
  state.rememberMeshName('game');
  state.setNativeActorRef(actorRef);
  const target = { routerChannelId: 'game', targetNodeRid: 'node-a', spotId: entry ? entrySpotId : 'room', spotKind: entry ? framework.ZLinkSpotKind.Entry : framework.ZLinkSpotKind.User, targetSpotGeneration: 1n };
  if (entry) {
    location = { ...location, spotId: 'room' };
    state.setJoinedSpot('room', undefined, 1n, 1n);
  }
  if (sameTarget) location = { ...location, spotId: target.spotId };
  let frameworkJoined = false;
  const node = {
    status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n }),
    entrySpot: () => ({ routingId: 'node-a', status: () => ({ lifecycleGeneration: 1n }) }),
    actorLookup: () => location,
    restoreActorAuthority(_id, _type, _generation, _owner, spotId, spotGeneration, membershipEpoch) {
      events.push('membership');
      location = { actor: actorRef, spotId: spotId ?? 'node-a', spotGeneration, membershipEpoch };
      return actorRef;
    },
    joinActorSpot() { events.push('mesh-record'); return { high: 1n, low: 1n }; },
    joinActorEntrySpot() { events.push('mesh-record'); return { high: 1n, low: 1n }; }
  };
  const publish = async (_actorType, _actorId, spotId, spotGeneration) => {
    events.push('store');
    storeMemberships.push({ spotId, spotGeneration });
    if (storeFailure) throw new Error('Store commit failed');
  };
  const joined = async () => {
    assert.equal(location.spotId, entry ? 'node-a' : target.spotId);
    events.push('joined');
    if (lifecycleFailure) throw new Error('OnJoinedActor failed');
    if (destroyAfterJoined) state.markNativeActorDestroyed(actorRef);
  };
  const activation = {
    spotId: target.spotId,
    meshName: 'game',
    serial: new ZLinkSpotSerialTurnExecutor(),
    actorHandlers: {},
    commitActorJoin() { frameworkJoined = true; return () => { frameworkJoined = false; }; },
    spot: {
      async onActorJoin() {
        events.push('admission');
        if (admissionWaitForAbort) {
          await new Promise(resolve => joinSignal.addEventListener('abort', resolve, { once: true }));
        }
        return { accepted: !rejected };
      },
      onJoinedActor: joined
    }
  };
  const membership = new ZLinkSpotActorMembership({ resolveActivation: () => activation });
  const coordinator = new framework.ZLinkActorNativeJoinCoordinator({
    node,
    completionTableProvider: () => ({
      async submit(submit) {
        submit();
        return { terminalResult: 0, failureErrno: 0, parts: [], kindData: { kind: 'actorJoinCompletion', actor: actorRef, joinResult: 0, location: { spotId: target.spotId, spotGeneration: 1n, membershipEpoch: 2n } } };
      }
    }),
    spotRouteResolver: { async resolve() { return target; } },
    entrySpotIdProvider: () => entrySpotId,
    locationLifecycle: { notifyActorJoinedSpot: publish, notifyActorLeftSpot: publish },
    localSpotJoin: (...args) => { joinSignal = args[4]; return membership.admitActorJoin(...args); },
    async localSourceLeave() { events.push('source-left'); await sourceLeave?.(); },
    localEntryJoin: joined,
    remoteActorBinder: postCommitBinder?.bind,
    reportSourceLeaveError(error) { events.push(`source-error:${error.message}`); },
    shutdownSignal: shutdown.signal
  });
  const context = new DefaultZLinkActorContext(state, coordinator, undefined, undefined, () => 'game');
  const actor = {
    context,
    async onJoinCompleted(completion) { completions.push(completion); events.push(`completion:${completion.status}`); }
  };
  state.getOrStartCreation('player', false, async () => ({ status: 'created', actor }));
  state.bindActor(actor, context);
  return { state, events, completions, storeMemberships, frameworkJoined: () => frameworkJoined, location: () => location, async run(timeoutMs) {
    try {
      await runActorHandlerWithDeferredJoins(() => {
        const call = entry ? context.joinEntrySpot() : context.joinSpot('room');
        if (timeoutMs !== undefined) call.timeout(timeoutMs);
        call.defer();
      });
    } finally { shutdown.abort(); }
  } };
}

for (const entry of [false, true]) {
  test(`same-node ${entry ? 'Entry' : 'User'} Join commits Store before membership, lifecycle and Accepted`, async () => {
    const f = fixture({ entry });
    await f.run();
    assert.deepEqual(f.events, [...(entry ? [] : ['admission']), 'store', 'membership', 'joined', 'source-left', 'completion:accepted']);
    assert.equal(f.location().membershipEpoch, 2n);
  });
  test(`same-node ${entry ? 'Entry' : 'User'} Store failure reaches public Failed completion`, async () => {
    const f = fixture({ entry, storeFailure: true });
    await f.run();
    assert.deepEqual(f.events, [...(entry ? [] : ['admission']), 'store', 'completion:failed']);
    assert.equal(f.location().membershipEpoch, 1n);
  });
}

test('same-node Entry Join preserves public Entry identity while committing native membership', async () => {
  const f = fixture({ entry: true, entrySpotId: 'game-entry-00000000-0000-4000-8000-000000000001' });
  await f.run();
  assert.deepEqual(f.events, ['store', 'membership', 'joined', 'source-left', 'completion:accepted']);
  assert.equal(f.location().spotId, 'node-a');
  assert.deepEqual(f.storeMemberships, [{ spotId: 'game-entry-00000000-0000-4000-8000-000000000001', spotGeneration: 1n }]);
});

test('same-node Entry Join retains committed identity when the joined callback destroys the Actor', async () => {
  const bindings = [];
  const f = fixture({ entry: true, destroyAfterJoined: true, postCommitBinder: { async bind(actor) { bindings.push(actor); } } });
  await f.run();
  assert.equal(f.state.nativeActorRef, undefined);
  assert.equal(f.completions[0].status, 'accepted');
  assert.deepEqual(bindings, []);
});

test('same-node lifecycle failure reports Failed while committed membership remains', async () => {
  const f = fixture({ lifecycleFailure: true });
  await f.run();
  assert.deepEqual(f.events, ['admission', 'store', 'membership', 'joined', 'source-left', 'completion:failed']);
  assert.equal(f.location().spotId, 'room');
  assert.equal(f.frameworkJoined(), true);
});

test('same-node rejection changes neither Store nor membership', async () => {
  const f = fixture({ rejected: true });
  await f.run();
  assert.deepEqual(f.events, ['admission', 'completion:rejected']);
  assert.equal(f.location().membershipEpoch, 1n);
});

for (const entry of [false, true]) {
  test(`same-node ${entry ? 'Entry' : 'User'} same-target Join completes without commit or lifecycle`, async () => {
    const f = fixture({ entry, sameTarget: true });
    await f.run();
    assert.deepEqual(f.events, ['completion:accepted']);
    assert.equal(f.location().membershipEpoch, 1n);
  });
}

for (const entry of [false, true]) {
  test(`same-node ${entry ? 'Entry' : 'User'} Join completion does not wait for one-way source leave`, async () => {
    let release;
    const leave = new Promise(resolve => { release = resolve; });
    const f = fixture({ entry, sourceLeave: () => leave });
    try {
      await f.run();
      assert.deepEqual(f.events, [...(entry ? [] : ['admission']), 'store', 'membership', 'joined', 'source-left', 'completion:accepted']);
    } finally { release(); }
  });

  test(`same-node ${entry ? 'Entry' : 'User'} source leave failure is reported without changing Accepted completion`, async () => {
    const f = fixture({ entry, sourceLeave: async () => { throw new Error('source leave failed'); } });
    await f.run();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(f.completions[0].status, 'accepted');
    assert.ok(f.events.includes('source-error:source leave failed'));
    assert.equal(f.location().membershipEpoch, 2n);
  });
}

test('same-node Join deadline expires during admission before Store commit', async () => {
  const f = fixture({ admissionWaitForAbort: true });
  await f.run(10);
  assert.deepEqual(f.events, ['admission', 'completion:failed']);
  assert.equal(f.location().membershipEpoch, 1n);
});

test('stateful local membership restoration preserves Session binding identity and updates its epoch', () => {
  const { ServiceStatefulRegistry } = require('../../packages/framework/dist/runtime/foundation/service-stateful-registry');
  const registry = new ServiceStatefulRegistry('node-a', 1n);
  const actor = registry.createActor('alice', 'player');
  const spot = registry.createSpot('room', 'user', 'room');
  const binding = registry.bindSession(actor.ref, 'session-a', 'node-a');
  registry.restoreActor(actor.ref, 'player', spot.ref, 2n, actor.authorityOwnerGeneration);
  assert.deepEqual(registry.binding(actor.ref), { ...binding, membershipEpoch: 2n });
});
