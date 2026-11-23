const assert = require('node:assert/strict');
const test = require('node:test');
const {
  currentZLinkActorExecution,
  runZLinkActorExecution
} = require('../../packages/framework/dist/runtime/actors/actor-execution-context');
const {
  ZLINK_ACTOR_LIFECYCLE_SNAPSHOT
} = require('../../packages/framework/dist/runtime/actors/actor-lifecycle-snapshot');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const { ZLinkExecutionBarrier } = require('../../packages/framework/dist/runtime/execution');
const {
  ZLinkSpotActorMembership
} = require('../../packages/framework/dist/runtime/spots/spot-actor-membership');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

test('Spot Join lifecycle does not inherit the Actor that wakes its queue', async () => {
  const actorRef = {
    actorId: 'casual-a',
    nodeRid: 'match',
    meshName: 'game',
    objectGeneration: 1n
  };
  const actor = {
    context: {
      actorId: actorRef.actorId,
      [ZLINK_ACTOR_LIFECYCLE_SNAPSHOT]: () => ({
        actorRef,
        actorType: 'player',
        membershipEpoch: 1n
      })
    }
  };
  const competingActor = new ZLinkActorRuntimeState('ranked-a');
  const serial = new ZLinkSpotSerialTurnExecutor();
  const barrier = new ZLinkExecutionBarrier();
  const callbacks = [];
  const callbackActorScopes = [];
  const spot = {
    onActorJoin() {
      callbackActorScopes.push(currentZLinkActorExecution());
      return { accepted: true };
    },
    onJoinedActor(joined) {
      callbackActorScopes.push(currentZLinkActorExecution());
      callbacks.push(joined);
    }
  };
  const policy = {
    executionDecision: (owner) => (owner === undefined ? 'lost' : 'allowed'),
    executionFailure: () => new Error('Object execution authority was discarded.')
  };
  barrier.setExecutionPolicy(
    () => policy,
    () => {
      const current = currentZLinkActorExecution();
      if (current === undefined) return spot;
      if (current.actorId === actorRef.actorId) return actor;
      assert.equal(current.actorId, competingActor.actorId);
      return competingActor.isMoving ? undefined : competingActor;
    }
  );
  serial.setExecutionBarrier(barrier);
  const activation = { serial, spot, commitActorJoin() {} };
  const membership = new ZLinkSpotActorMembership({ resolveActivation: () => activation });

  const request = ZLinkBufferMessage.from(Buffer.alloc(0));
  try {
    const result = await runZLinkActorExecution(competingActor.actorId, 'lobby', () => {
      // The first submission schedules the Spot consumer in this Actor's scope.
      const wake = serial.executeControlLifecycleOperation(() => {});
      const join = runZLinkActorExecution(actorRef.actorId, 'lobby', () =>
        membership.admitActorJoin('room-2', actor, request, () => competingActor.beginMove())
      );
      return Promise.all([wake, join]).then(([, outcome]) => outcome);
    });
    assert.equal(result.accepted, true);
    assert.deepEqual(callbacks, [actor]);
    assert.deepEqual(callbackActorScopes, [undefined, undefined]);
  } finally {
    request.close();
  }
});
