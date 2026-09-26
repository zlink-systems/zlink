const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');

// Spot Actor membership sections 4-5: the public Join OperationId, the
// optional reply and the delivery state live only while the current source
// and target processes run. Nothing is written to a Store, so nothing can be
// replayed after a restart.

function touchedStore(calls, name) {
  return new Proxy(
    {},
    {
      get(_target, property) {
        return (..._args) => {
          calls.push(`${name}.${String(property)}`);
          throw new Error(`${name} must not be used for a Join completion.`);
        };
      }
    }
  );
}

test('deferred Join Accepted completion is process-local and runs its callback once', async () => {
  const storeCalls = [];
  const runtime = new ZLinkActorTransferRuntime({
    authorityStore: () => touchedStore(storeCalls, 'authorityStore'),
    relocationStore: () => touchedStore(storeCalls, 'relocationStore'),
    actorManager: () => ({ getState: () => undefined })
  });
  const actorRef = {
    actorId: 'player-1',
    objectGeneration: 7n,
    meshName: 'play',
    nodeRid: 'target-node'
  };
  const operationId = { high: 0x11n, low: 0x22n };
  const completions = [];
  const actor = {
    context: { actorId: 'player-1', meshName: 'play' },
    async onJoinCompleted(completion) {
      completions.push(completion);
    }
  };
  let mailboxTurns = 0;
  const submitMailbox = async (operation) => {
    mailboxTurns += 1;
    return await operation();
  };

  const completion = runtime.prepareDeferredJoinAccepted(
    'player-1',
    operationId,
    actorRef,
    Buffer.alloc(0)
  );
  await runtime.deliverDeferredJoinAccepted(completion, actor, actorRef, submitMailbox);
  await runtime.deliverDeferredJoinAccepted(completion, actor, actorRef, submitMailbox);

  assert.deepEqual(storeCalls, []);
  assert.equal(mailboxTurns, 1);
  assert.equal(completions.length, 1);
  assert.equal(completions[0].status, 'accepted');
  assert.deepEqual(completions[0].operationId, operationId);
  assert.equal(completions[0].actor.actorId, 'player-1');
  assert.equal(completions[0].reply, undefined);
});

test('deferred Join handoff is not keyed by the public Join OperationId', () => {
  const handoffCalls = [];
  const sourceTransfer = {
    beginDeferredActorHandoff(...args) {
      handoffCalls.push(args);
    },
    async cancelDeferredActorHandoff(...args) {
      handoffCalls.push(args);
    }
  };
  const coordinator = new framework.ZLinkActorNativeJoinCoordinator({
    node: {},
    completionTableProvider: () => undefined,
    sourceTransfer
  });
  const actor = { context: { actorId: 'player-1' } };
  const state = new ZLinkActorRuntimeState('player-1');
  const operationId = { high: 0x33n, low: 0x44n };

  coordinator.beginDeferredJoin(actor, state, operationId);
  void coordinator.abortDeferredJoin(actor, state, operationId);

  assert.equal(handoffCalls.length, 2);
  for (const args of handoffCalls) {
    assert.deepEqual(args, [actor, state]);
  }
});
