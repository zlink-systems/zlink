const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkSpotActorMembership } = require('../../packages/framework/dist/runtime/spots/spot-actor-membership');
const { DefaultZLinkActorContext } = require('../../packages/framework/dist/runtime/actors/actor-context');
const { ZLinkActorRuntimeState } = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const { ZLinkEntryActorRuntimeService } = require('../../packages/framework/dist/runtime/host/entry-actor-runtime');
const { ZLinkActorPacketRelay } = require('../../packages/framework/dist/runtime/host/actor-packet-relay');
const { createSpotContext } = require('../../packages/framework/dist/runtime/spots/spot-context');

function fixture({ sourceLeave = async () => {}, storeFailure = false } = {}) {
  const events = [];
  const actorRef = { actorId: 'alice', nodeRid: 'node-a', generation: 1n };
  let location = { actor: actorRef, spotId: 'room', spotGeneration: 1n, membershipEpoch: 1n };
  const state = new ZLinkActorRuntimeState('alice');
  state.rememberMeshName('game');
  state.setNativeActorRef(actorRef);
  state.setJoinedSpot('room', undefined, 1n, 1n);
  state.setRemoteActorPacketTarget({ routerChannelId: 'game', targetNodeRid: 'node-a', spotId: 'room' });
  const clearedTargets = [];
  let reportError;
  const reported = new Promise(( resolve ) => { reportError = resolve; });
  let sourceStarted;
  const notified = new Promise(( resolve ) => { sourceStarted = resolve; });
  const source = {
    spotId: 'room',
    meshName: 'game',
    serial: new framework.ZLinkSpotSerialTurnExecutor(),
    beginActorTransfer() { events.push('source-begin'); },
    commitActorDeparture() { events.push('source-departure'); },
    spot: {
      async onLeaveActor() {
        assert.equal(location.spotId, 'node-a', 'source notification requires committed Entry membership');
        events.push('source-left');
        sourceStarted();
        await sourceLeave();
      }
    }
  };
  const membership = new ZLinkSpotActorMembership({
    resolveActivation: () => source,
    entryNodeRid: 'node-a',
    actorTransferRuntime: {
      actorEntryNodeRid: () => 'node-a',
      clearRoutedActor() { events.push('source-clear'); state.clearJoinedSpot(); }
    }
  });
  const node = {
    status: () => ({ routingId: 'node-a', lifecycleGeneration: 1n }),
    actorLookup: () => location,
    entrySpot: () => ({ routingId: 'node-a', status: () => ({ lifecycleGeneration: 1n }) }),
    restoreActorAuthority(_id, _type, _generation, _owner, spotId, spotGeneration, membershipEpoch) {
      events.push('membership');
      location = { actor: actorRef, spotId: spotId ?? 'node-a', spotGeneration, membershipEpoch };
      return actorRef;
    }
  };
  const packetRelay = new ZLinkActorPacketRelay({
    actorManager: () => ({ getState: () => state }),
    streamBindingRuntime: () => ({ async find() { return undefined; } }),
    meshRouters: {
      defaultSpotRouterChannelId: () => 'game',
      defaultRouterChannelId: () => 'game'
    },
    spotNodeRuntime: () => ({ primaryMeshNode: node })
  });
  const entryRuntime = new ZLinkEntryActorRuntimeService({
    actorManager: () => ({ getState: () => state }),
    spotManager: () => ({ async dispatchRoutedActorPacket(_origin ) { return 'room'; } }),
    spotNodeRuntime: () => ({ primaryMeshNode: node }),
    streamBindingRuntime: { async refreshActor() {} },
    boundSessionRelay: {
      clearRemoteActorPacketTarget(actorId) {
        clearedTargets.push(actorId);
        packetRelay.clearRemoteActorPacketTarget(actorId);
      }
    }
  });
  const coordinator = new framework.ZLinkActorNativeJoinCoordinator({
    node,
    entrySpotIdProvider: () => 'node-a',
    locationLifecycle: {
      async notifyActorLeftSpot() {
        events.push('store');
        if (storeFailure) throw new Error('Store commit failed');
      }
    },
    localEntryJoin: ( actor ) => entryRuntime.commitActorTransaction(actor, async () => { events.push('joined'); }),
    localSourceLeave: (actor, spotId) => {
      assert.equal(spotId, 'room', 'canonical Join retains the source membership');
      return membership.notifyActorLeftAfterTransfer(spotId, actor);
    },
    reportSourceLeaveError(error) { events.push('source-error'); reportError(error); }
  });
  const context = new DefaultZLinkActorContext(state, coordinator, undefined, undefined, () => 'game');
  const actor = { context };
  state.getOrStartCreation('player', false, async () => ({ status: 'created', actor }));
  state.bindActor(actor, context);
  const spotContext = createSpotContext({
    meshName: 'game',
    spotId: 'room',
    objectGeneration: 1,
    nodeRid: 'node-a',
    leaveActor: (leavingActor, signal) => membership.leaveActor('room', leavingActor, signal)
  });
  return { events, reported, notified, context, packetRelay, entryRuntime, clearedTargets, leave: () => spotContext.leaveActor(actor), location: () => location };
}

test('Spot context leave restores Entry state and clears the previous packet route', async () => {
  const f = fixture();
  assert.equal(f.context.spotId, 'room');
  assert.equal((await f.entryRuntime.routePacket('alice', [])).handled, true);
  assert.equal(f.packetRelay.actorPacketTargetForState('alice').spotId, 'room');
  await f.leave();
  assert.equal(f.location().spotId, 'node-a');
  assert.equal(f.context.spotId, undefined);
  assert.equal((await f.entryRuntime.routePacket('alice', [])).handled, false);
  assert.equal(f.packetRelay.actorPacketTargetForState('alice'), undefined);
  assert.deepEqual(f.clearedTargets, ['alice']);
});

test('Spot context leave commits Entry membership before notifying source once', async () => {
  const f = fixture();
  await f.leave();
  await f.notified;
  assert.deepEqual(f.events.slice(0, 5), ['store', 'membership', 'joined', 'source-begin', 'source-left']);
  assert.equal(f.events.filter(( event ) => event === 'source-left').length, 1);
  assert.equal(f.events.includes('source-clear'), false);
});

test('Spot context leave completes while source notification remains pending', async () => {
  let release;
  const pending = new Promise(( resolve ) => { release = resolve; });
  const f = fixture({ sourceLeave: () => pending });
  try {
    await f.leave();
    await f.notified;
    assert.equal(f.location().spotId, 'node-a');
    assert.equal(f.events.filter(( event ) => event === 'source-left').length, 1);
    assert.equal(f.events.includes('source-departure'), false);
  } finally { release(); }
});

test('Spot context leave source failure is reported without rejecting committed Join', async () => {
  const failure = new Error('source leave failed');
  const f = fixture({ sourceLeave: async () => { throw failure; } });
  await f.leave();
  assert.equal(await f.reported, failure);
  assert.equal(f.location().spotId, 'node-a');
  assert.equal(f.events.filter(( event ) => event === 'source-left').length, 1);
  assert.equal(f.events.filter(( event ) => event === 'source-error').length, 1);
});

test('Spot context leave Store failure preserves source and does not notify it', async () => {
  const f = fixture({ storeFailure: true });
  await assert.rejects(f.leave(), /Store commit failed/);
  assert.deepEqual(f.events, ['store']);
  assert.equal(f.location().spotId, 'room');
});
