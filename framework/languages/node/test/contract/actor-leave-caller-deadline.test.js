const assert = require('node:assert/strict');
const test = require('node:test');
const {
  DefaultZLinkActorContext,
  ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME
} = require('../../packages/framework/dist/runtime/actors/actor-context');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const {
  ZLinkSpotActorMembership
} = require('../../packages/framework/dist/runtime/spots/spot-actor-membership');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const { createSpotContext } = require('../../packages/framework/dist/runtime/spots/spot-context');

function latch() {
  let release;
  const promise = new Promise((resolve) => {
    release = resolve;
  });
  return { promise, release };
}
async function flushTurns() {
  for (let i = 0; i < 20; i++) await new Promise((resolve) => setImmediate(resolve));
}
function actorWithCoordinator(joinEntrySpot) {
  const state = new ZLinkActorRuntimeState('caller-actor');
  const context = new DefaultZLinkActorContext(
    state,
    { joinEntrySpot },
    undefined,
    undefined,
    () => 'mesh'
  );
  const actor = { context };
  state.bindActor(actor, context);
  return { actor, context };
}

test('public Spot leave caller cancellation reacquires its turn while original Entry lifecycle retains cleanup and late reply', async () => {
  const started = latch(),
    admission = latch(),
    cleanupStarted = latch(),
    cleanup = latch();
  let coreSignal,
    replyClosed = 0,
    requestClosed = 0,
    waiterState = 'pending',
    lifecycleFinished = false;
  const { actor } = actorWithCoordinator(
    async (_actor, _state, _node, encoded, _timeout, signal) => {
      const closeRequest = encoded.close.bind(encoded);
      encoded.close = () => {
        requestClosed++;
        closeRequest();
      };
      coreSignal = signal;
      started.release();
      await admission.promise;
      return {
        accepted: false,
        reply: {
          close() {
            replyClosed++;
          }
        },
        sourceCleanup: async () => {
          cleanupStarted.release();
          await cleanup.promise;
        }
      };
    }
  );
  const serial = new ZLinkSpotSerialTurnExecutor();
  const activation = { serial, spot: {} };
  const membership = new ZLinkSpotActorMembership({
    resolveActivation: () => activation,
    entryNodeRid: 'entry'
  });
  const context = createSpotContext({
    meshName: 'mesh',
    spotId: 'room',
    objectGeneration: 1n,
    serial,
    getSpot: () => activation.spot,
    leaveActor: (a, signal) => membership.leaveActor('room', a, signal, 'mesh')
  });
  const controller = new AbortController();
  const leaving = serial.execute(async () => {
    try {
      await context.leaveActor(actor, controller.signal);
      waiterState = 'fulfilled';
    } catch (error) {
      assert.equal(error.name, 'AbortError');
      assert.equal(serial.isCurrentTurn, true, 'caller resumed without reacquiring its turn');
      waiterState = 'aborted';
    }
  });
  let later;
  try {
    await started.promise;
    const otherEntered = latch(),
      otherHeld = latch();
    const other = serial.execute(async () => {
      otherEntered.release();
      await otherHeld.promise;
    });
    await otherEntered.promise;
    controller.abort();
    await flushTurns();
    assert.equal(waiterState, 'pending', 'caller overtook the currently owned turn');
    otherHeld.release();
    await other;
    await flushTurns();
    assert.equal(
      waiterState,
      'aborted',
      'public waiter did not end independently of original lifecycle'
    );
    assert.equal(coreSignal, undefined, 'application signal entered the original operation');
    assert.equal(replyClosed, 0);
    assert.equal(requestClosed, 0, 'underlying request closed before lifecycle terminal');
    later = serial.executeLifecycleOperation(() => {
      lifecycleFinished = true;
    });
    admission.release();
    await cleanupStarted.promise;
    await flushTurns();
    assert.equal(lifecycleFinished, false, 'lifecycle FIFO released before source cleanup');
    assert.equal(replyClosed, 0);
    assert.equal(requestClosed, 0, 'underlying request closed before lifecycle terminal');
    cleanup.release();
    await later;
    assert.equal(replyClosed, 1, 'late owned reply must close exactly once');
    assert.equal(requestClosed, 1, 'underlying request must close exactly once after cleanup');
  } finally {
    admission.release();
    cleanup.release();
    await leaving;
    await later;
    await serial.executeLifecycleOperation(() => {});
  }
});

for (const stage of ['sourceCleanup', 'finalizeDeferredJoin']) {
  test(`Entry runtime consumes ${stage} terminal and propagates its error after owned reply closes`, async () => {
    const entered = latch(),
      held = latch();
    const failure = new Error(stage);
    let closed = 0,
      terminal = false;
    const { context } = actorWithCoordinator(async () => ({
      accepted: false,
      reply: {
        close() {
          closed++;
        }
      },
      [stage]: async () => {
        entered.release();
        await held.promise;
        throw failure;
      }
    }));
    const operation = context[ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME]('entry', undefined);
    const observed = operation.then(
      () => {
        terminal = true;
      },
      () => {
        terminal = true;
      }
    );
    try {
      await flushTurns();
      assert.equal(terminal, false, 'Entry runtime returned before owned lifecycle terminal');
      assert.equal(closed, stage === 'sourceCleanup' ? 0 : 1);
    } finally {
      held.release();
      await observed;
    }
    await assert.rejects(operation, (error) => error === failure);
    assert.equal(closed, 1);
  });
}

for (const entry of [false, true]) {
  test(`deferred ${entry ? 'Entry' : 'User'} Join carries its first absolute deadline through route lookup and each native submission`, async () => {
    const {
      ZLinkActorNativeJoinCoordinator
    } = require('../../packages/framework/dist/runtime/actors/actor-remote-joiner');
    const {
      runActorHandlerWithDeferredJoins
    } = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
    const {
      ZLinkBufferMessage
    } = require('../../packages/framework/dist/runtime/backend/runtime-message');
    const {
      ZLinkBackendResultError,
      SubmitResult
    } = require('../../packages/framework/dist/runtime/backend/runtime-values');
    const calls = [];
    let D0, hostD, before;
    const ref = { actorId: 'deadline-actor', nodeRid: 'source', generation: 1n };
    const node = {
      status: () => ({ routingId: 'source', lifecycleGeneration: 1n }),
      joinActorSpot(...args) {
        return submit(args);
      },
      joinActorEntrySpot(...args) {
        return submit(args);
      }
    };
    function submit(args) {
      calls.push({ before, after: performance.now(), timeout: args.at(-1) });
      if (calls.length === 1)
        throw new ZLinkBackendResultError('submit', SubmitResult.NotConnected);
      return { high: 1n, low: 1n };
    }
    const state = new ZLinkActorRuntimeState(ref.actorId);

    state.setNativeActorRef(ref);
    const coordinator = new ZLinkActorNativeJoinCoordinator({
      node,
      completionTableProvider: () => ({
        submit(callback) {
          before = performance.now();
          callback();
          return Promise.resolve({
            terminalResult: 0,
            failureErrno: 0,
            kindData: { kind: 'actorJoinCompletion', actor: ref, joinResult: 0 },
            parts: [ZLinkBufferMessage.from('reply')]
          });
        }
      }),
      entrySpotIdProvider: () => 'entry',
      spotRouteResolver: {
        async resolve(spotId) {
          await new Promise((resolve) => setTimeout(resolve, 20));
          return {
            routerChannelId: 'mesh',
            targetNodeRid: 'target',
            targetSpotGeneration: 1n,
            spotId
          };
        }
      },
      actorJoinRelocation: {
        async relocateActorJoin(input) {
          hostD = input.deadlineAtMs;
          return { sourceCleanup: async () => {} };
        }
      }
    });
    const method = entry ? 'joinEntrySpot' : 'joinSpot';
    const original = coordinator[method].bind(coordinator);
    coordinator[method] = (...args) => {
      D0 = args[7];
      return original(...args);
    };
    const context = new DefaultZLinkActorContext(
      state,
      coordinator,
      undefined,
      undefined,
      () => 'mesh'
    );
    const actor = { context, async onJoinCompleted() {} };
    state.getOrStartCreation('player', false, async () => ({ status: 'created', actor }));
    state.bindActor(actor, context);
    await runActorHandlerWithDeferredJoins(() => {
      (entry ? context.joinEntrySpot() : context.joinSpot('room')).timeout(30_000).defer();
    });
    assert.equal(calls.length, 2, 'fixture must reach both native readiness submissions');
    assert.equal(typeof hostD, 'number', 'fixture must reach Host relocation input');
    assert.equal(typeof D0, 'number', 'first deferred absolute deadline was not forwarded');
    assert.equal(hostD, D0, 'route lookup reconstructed a later Host deadline');
    assert.equal(
      calls.length,
      2,
      'one synchronous readiness retry followed by one physical request'
    );
    for (const call of calls) {
      assert.ok(call.timeout > 0);
      assert.ok(
        Math.ceil(D0 - call.after) <= call.timeout && call.timeout <= Math.ceil(D0 - call.before),
        'native submission used stale relative timeout outside its clock bracket'
      );
    }
    assert.ok(calls[1].timeout < calls[0].timeout, 'readiness retry reused stale native budget');
  });
}

test('public Spot leave with an already aborted caller starts no lifecycle or Entry resolution', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor();
  const membership = new ZLinkSpotActorMembership({
    resolveActivation() {
      assert.fail('pre-aborted leave must not resolve an activation');
    }
  });
  const context = createSpotContext({
    meshName: 'mesh',
    spotId: 'room',
    objectGeneration: 1n,
    serial,
    getSpot: () => ({}),
    leaveActor: (actor, signal) => membership.leaveActor('room', actor, signal, 'mesh')
  });
  const caller = new AbortController();
  caller.abort();
  await assert.rejects(context.leaveActor({}, caller.signal), { name: 'AbortError' });
});
