import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ZLinkUserSpotExecutionMode } from '../../packages/framework/src/contracts/Configuration/ObjectRoles';
import { ZLinkExecutionBarrier } from '../../packages/framework/src/runtime/execution';
import { ZLinkSpotActivationLifecycle } from '../../packages/framework/src/runtime/spots/spot-activation';
import { ZLinkSpotActivation } from '../../packages/framework/src/runtime/spots/spot-activation-state';
import { ZLinkSpotSerialExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-executor';
import { ZLinkSpotTimerRegistry } from '../../packages/framework/src/runtime/spots/spot-timer';

interface Deferred {
  readonly promise: Promise<void>;
  resolve(): void;
}

function deferred(): Deferred {
  let resolve!: () => void;
  const promise = new Promise<void>((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

function activation(
  serial: ZLinkSpotSerialExecutor,
  timers: ZLinkSpotTimerRegistry,
  executionMode: ZLinkUserSpotExecutionMode
): ZLinkSpotActivation {
  return new ZLinkSpotActivation({
    meshName: 'mesh',
    spotId: 'spot-barrier' as never,
    spotType: class BarrierSpot {} as never,
    spot: {} as never,
    serial,
    executionMode,
    timers,
    actorHandlers: {} as never,
    handlers: {} as never
  });
}

test('lifecycle seal waits for a yielded Promise continuation and holds later Spot turns', async () => {
  const barrier = new ZLinkExecutionBarrier();
  const serial = new ZLinkSpotSerialExecutor(true);
  serial.setExecutionBarrier(barrier);
  const response = deferred();
  const started = deferred();
  const events: string[] = [];

  const yielded = serial.execute(async () => {
    events.push('yielded:start');
    started.resolve();
    await serial.yieldPromise(response.promise);
    events.push('yielded:complete');
  });
  await started.promise;
  await serial.execute(() => events.push('before-seal'));

  const seal = barrier.seal();
  const held = serial.post(() => events.push('after-abort'));
  let quiescent = false;
  const waiting = barrier.waitForQuiescence(seal).then(() => {
    quiescent = true;
  });
  await Promise.resolve();
  assert.equal(quiescent, false);
  assert.deepEqual(events, ['yielded:start', 'before-seal']);

  response.resolve();
  await yielded;
  await waiting;
  assert.equal(quiescent, true);
  assert.deepEqual(events, ['yielded:start', 'before-seal', 'yielded:complete']);

  assert.equal(barrier.abort(seal), true);
  await held;
  assert.deepEqual(events, [
    'yielded:start',
    'before-seal',
    'yielded:complete',
    'after-abort'
  ]);
});

test('PerActor lifecycle barrier quiesces Actor, Spot, and timer lanes together', async () => {
  const spotSerial = new ZLinkSpotSerialExecutor(false);
  const timerSerials = new Map<string, ZLinkSpotSerialExecutor>();
  const timers = new ZLinkSpotTimerRegistry(undefined, () => false, (name) => {
    let serial = timerSerials.get(name);
    if (serial === undefined) {
      serial = new ZLinkSpotSerialExecutor(false);
      timerSerials.set(name, serial);
    }
    return serial;
  });
  const state = activation(spotSerial, timers, ZLinkUserSpotExecutionMode.PerActor);
  const actorDone = deferred();
  const actorStarted = deferred();
  const spotDone = deferred();
  const spotStarted = deferred();
  const timerDone = deferred();
  const timerStarted = deferred();

  const actor = state.executeActor('actor-a', serial => serial.execute(async () => {
    actorStarted.resolve();
    await actorDone.promise;
  }));
  const spot = spotSerial.execute(async () => {
    spotStarted.resolve();
    await spotDone.promise;
  });
  class TimerHandler {
    async handle(): Promise<void> {
      timerStarted.resolve();
      await timerDone.promise;
    }
  }
  const timer = await timers.add(
    'heartbeat',
    1,
    undefined,
    TimerHandler as never,
    spotSerial,
    {} as never
  );
  await Promise.all([actorStarted.promise, spotStarted.promise, timerStarted.promise]);

  const seal = state.sealExecution();
  let quiescent = false;
  const waiting = state.waitForExecutionQuiescence(seal).then(() => {
    quiescent = true;
  });
  await Promise.resolve();
  assert.equal(quiescent, false);

  const cancelTimer = timer.cancel();
  actorDone.resolve();
  spotDone.resolve();
  timerDone.resolve();
  await Promise.all([actor, spot, cancelTimer, waiting]);
  assert.equal(quiescent, true);
  assert.equal(state.abortExecutionSeal(seal), true);
  await timers.dispose();
});

test('Spot close invokes lifecycle cleanup only after its execution seal is quiescent', async () => {
  const serial = new ZLinkSpotSerialExecutor(true);
  const timers = new ZLinkSpotTimerRegistry();
  const events: string[] = [];
  const activeDone = deferred();
  const activeStarted = deferred();
  const state = new ZLinkSpotActivation({
    meshName: 'mesh',
    spotId: 'spot-close-barrier' as never,
    spotType: class ClosingSpot {} as never,
    spot: {
      async onClosing() {
        events.push('closing');
      }
    } as never,
    serial,
    timers,
    actorHandlers: {} as never,
    handlers: {} as never
  });
  const lifecycle = new ZLinkSpotActivationLifecycle({
    locationClaim: {
      async release() {
        assert.fail('The lifecycle release port owns location cleanup.');
      }
    },
    releaseLocation: async () => {
      events.push('released');
    },
    leaveActor: async () => undefined,
    closeSpot: async () => true,
    registerActivation: () => undefined
  } as never);

  const active = serial.execute(async () => {
    events.push('active:start');
    activeStarted.resolve();
    await activeDone.promise;
    events.push('active:complete');
  });
  await activeStarted.promise;

  const seal = state.sealExecution();
  const closing = lifecycle.closeAfterSeal(state, seal);
  await Promise.resolve();
  assert.deepEqual(events, ['active:start']);

  activeDone.resolve();
  await Promise.all([active, closing]);
  assert.deepEqual(events, [
    'active:start',
    'active:complete',
    'closing',
    'released'
  ]);
  await assert.rejects(
    () => serial.post(() => undefined),
    /execution barrier is committed/
  );
});

test('relocation abort restores only its exact barrier generation', async () => {
  const serial = new ZLinkSpotSerialExecutor(false);
  const timers = new ZLinkSpotTimerRegistry();
  const state = activation(serial, timers, ZLinkUserSpotExecutionMode.PerActor);

  const first = await state.captureRelocation();
  assert.equal(state.abortRelocation(first), true);

  const second = await state.captureRelocation();
  let admitted = false;
  const held = serial.post(() => {
    admitted = true;
  });
  await Promise.resolve();
  assert.equal(state.abortRelocation(first), false);
  assert.equal(admitted, false);

  assert.equal(state.abortRelocation(second), true);
  await held;
  assert.equal(admitted, true);
  await timers.dispose();
});
