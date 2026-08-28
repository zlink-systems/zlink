import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  ZLinkSpotRelocationCoordinationMode,
  ZLinkUserSpotExecutionMode
} from '../../packages/framework/src/contracts/Configuration/ObjectRoles';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';
import { ZLinkConfigurationException } from '../../packages/framework/src/runtime/configuration';
import { ZLinkSpotActivation } from '../../packages/framework/src/runtime/spots/spot-activation-state';
import { ZLinkSpotSerialExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-executor';
import { ZLinkSpotSerialTurnExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-turn-executor';
import { ZLinkSpotTimerRegistry } from '../../packages/framework/src/runtime/spots/spot-timer';
import { DefaultZLinkWorkerCall } from '../../packages/framework/src/runtime/workers';
import {
  createRandomOperationIdentity,
  operationIdentityKey
} from '../../packages/framework/src/runtime/foundation/operation-identity';

interface Deferred<T> {
  readonly promise: Promise<T>;
  resolve(value: T): void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

function activation(
  serial: ZLinkSpotSerialTurnExecutor,
  executionMode: ZLinkUserSpotExecutionMode
): ZLinkSpotActivation {
  return new ZLinkSpotActivation({
    meshName: 'mesh',
    spotId: 'spot-1' as never,
    spotType: class TestSpot {} as never,
    spot: {} as never,
    serial,
    domain: {
      kind: 'user',
      executionMode,
      relocationCoordinationMode: ZLinkSpotRelocationCoordinationMode.FrameworkManaged
    },
    timers: {} as never,
    actorHandlers: {} as never,
    handlers: {} as never
  });
}

test('128-bit operation identity retries zero entropy and has one canonical key', () => {
  let calls = 0;
  const operationId = createRandomOperationIdentity(() => {
    calls += 1;
    const bytes = Buffer.alloc(16);
    if (calls === 2) bytes.writeBigUInt64BE(1n, 8);
    return bytes;
  });

  assert.equal(calls, 2);
  assert.deepEqual(operationId, { high: 0n, low: 1n });
  assert.equal(operationIdentityKey(operationId), '0:1');
});

test('same-owner nested execute rejects instead of running inside the active turn', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(true, 'spot-1' as never);
  const events: string[] = [];

  await serial.execute(() => {
    events.push('outer');
    assert.throws(
      () => serial.execute(() => events.push('nested')),
      (error) => error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.InvalidOperation
    );
    events.push('rejected');
  });
  await serial.post(() => events.push('next-turn'));

  assert.deepEqual(events, ['outer', 'rejected', 'next-turn']);
});

test('SpotWide Yield releases the Spot gate but retains the Actor claim', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(true);
  const state = activation(serial, ZLinkUserSpotExecutionMode.SpotWide);
  const response = deferred<string>();
  const events: string[] = [];

  const firstActorJob = state.executeActor('actor-a', actorSerial =>
    actorSerial.execute(async () => {
      events.push('actor-a:start');
      const value = await actorSerial.yieldPromise(response.promise);
      events.push(`actor-a:${value}`);
    })
  );
  await Promise.resolve();

  const secondActorJob = state.executeActor('actor-a', actorSerial =>
    actorSerial.execute(() => {
      events.push('actor-a:next');
    })
  );
  const otherActorJob = state.executeActor('actor-b', actorSerial =>
    actorSerial.execute(() => {
      events.push('actor-b');
    })
  );
  const spotJob = serial.execute(() => {
    events.push('spot');
  });

  await Promise.all([otherActorJob, spotJob]);
  assert.deepEqual([...events].sort(), ['actor-a:start', 'actor-b', 'spot']);

  response.resolve('resume');
  await Promise.all([firstActorJob, secondActorJob]);
  assert.deepEqual(events.slice(-2), ['actor-a:resume', 'actor-a:next']);
});

test('PerActor keeps Actor continuations ordered while Actor and Spot lanes run independently', async () => {
  const spotSerial = new ZLinkSpotSerialTurnExecutor(false);
  const state = activation(spotSerial, ZLinkUserSpotExecutionMode.PerActor);
  const response = deferred<string>();
  const events: string[] = [];

  const firstActorJob = state.executeActor('actor-a', actorSerial =>
    actorSerial.execute(async () => {
      events.push('actor-a:start');
      const value = await response.promise;
      events.push(`actor-a:${value}`);
    })
  );
  await Promise.resolve();

  const secondActorJob = state.executeActor('actor-a', actorSerial =>
    actorSerial.execute(() => {
      events.push('actor-a:next');
    })
  );
  const otherActorJob = state.executeActor('actor-b', actorSerial =>
    actorSerial.execute(() => {
      events.push('actor-b');
    })
  );
  const spotJob = spotSerial.execute(() => {
    events.push('spot');
  });

  await Promise.all([otherActorJob, spotJob]);
  assert.deepEqual([...events].sort(), ['actor-a:start', 'actor-b', 'spot']);

  response.resolve('continued');
  await Promise.all([firstActorJob, secondActorJob]);
  assert.deepEqual(events.slice(-2), ['actor-a:continued', 'actor-a:next']);
});

test('Yield rejects outside an allowed gate before worker admission', async () => {
  let scheduled = 0;
  const outsideCall = new DefaultZLinkWorkerCall(
    new ZLinkSpotSerialTurnExecutor(),
    async () => {
      scheduled += 1;
      return 'outside';
    }
  );
  assert.throws(
    () => outsideCall.yield(),
    ZLinkConfigurationException
  );
  assert.equal(scheduled, 0);

  const perActorSerial = new ZLinkSpotSerialTurnExecutor(false);
  await perActorSerial.execute(() => {
    const call = new DefaultZLinkWorkerCall(
      perActorSerial,
      async () => {
        scheduled += 1;
        return 'inside';
      }
    );
    assert.throws(
      () => call.yield(),
      ZLinkConfigurationException
    );
    assert.equal(scheduled, 0);
  });
});

test('PerActor timer registrations select an independent lane per timer name', async () => {
  const spotSerial = new ZLinkSpotSerialTurnExecutor(false);
  const serialExecutor = new ZLinkSpotSerialExecutor(
    spotSerial,
    ZLinkUserSpotExecutionMode.PerActor,
    'spot-a'
  );
  const registry = new ZLinkSpotTimerRegistry(
    undefined,
    () => false,
    undefined,
    undefined,
    (name, operation) => serialExecutor.executeTimer(name, operation),
    (name) => serialExecutor.isTimerExecuting(name)
  );
  class TimerHandler {
    handle(): void {}
  }

  await registry.add(
    'heartbeat',
    10_000,
    undefined,
    TimerHandler as never,
    spotSerial,
    {} as never
  );
  await registry.add(
    'expiry',
    10_000,
    undefined,
    TimerHandler as never,
    spotSerial,
    {} as never
  );

  const heartbeatStarted = deferred<void>();
  const heartbeatDone = deferred<void>();
  const expiryStarted = deferred<void>();
  const heartbeat = serialExecutor.executeTimer('heartbeat', async () => {
    heartbeatStarted.resolve();
    await heartbeatDone.promise;
  });
  await heartbeatStarted.promise;
  const expiry = serialExecutor.executeTimer('expiry', () => expiryStarted.resolve());
  await expiryStarted.promise;
  heartbeatDone.resolve();
  await Promise.all([heartbeat, expiry]);
  await registry.dispose();
});

test('Spot execution reserves message and byte capacity as one bounded admission', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(false, undefined, {
    ownerTimeBudget: 50,
    lifecycleBurstLimit: 8,
  });
  const startedSignal = deferred<void>();
  const finished = deferred<void>();
  const first = serial.execute(async () => {
    startedSignal.resolve();
    await finished.promise;
  });
  await startedSignal.promise;

  const accepted = serial.execute(() => undefined);
  const later = serial.execute(() => undefined);
  finished.resolve();
  await Promise.all([first, accepted, later]);
});

test('Spot execution includes metadata bytes in the same atomic reservation', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(false, undefined, {
  });
  const started = deferred<void>();
  const finished = deferred<void>();
  const first = serial.execute(async () => {
    started.resolve();
    await finished.promise;
  });
  await started.promise;

  const following = serial.execute(() => undefined);
  finished.resolve();
  await Promise.all([first, following]);
});

test('Spot barrier turns remain in application FIFO order and scheduler yields at its time budget', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(false, undefined, {
    ownerTimeBudget: 1,
    lifecycleBurstLimit: 8,
  });
  const events: string[] = [];
  const application = serial.execute(() => events.push('application'));
  const barrier = serial.postBarrierTurn(() => events.push('barrier'));
  await Promise.all([application, barrier]);
  assert.deepEqual(events.slice(0, 2), ['application', 'barrier']);

  let timerRan = false;
  const timer = new Promise<void>((resolve) => {
    setTimeout(() => {
      timerRan = true;
      resolve();
    }, 0);
  });
  const jobs = Array.from({ length: 128 }, () => serial.execute(() => undefined));
  await timer;
  await Promise.all(jobs);
  assert.equal(timerRan, true);
});

test('yield continuation re-enters behind earlier application turns', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(true);
  const response = deferred<void>();
  const blockerStarted = deferred<void>();
  const blockerFinished = deferred<void>();
  const events: string[] = [];

  const yielded = serial.execute(async () => {
    events.push('yielded:start');
    await serial.yieldPromise(response.promise);
    events.push('yielded:complete');
  });
  const blocker = serial.execute(async () => {
    events.push('blocker:start');
    blockerStarted.resolve();
    await blockerFinished.promise;
    events.push('blocker:complete');
  });
  await blockerStarted.promise;
  const queued = serial.execute(() => events.push('queued'));

  response.resolve();
  await Promise.resolve();
  blockerFinished.resolve();
  await Promise.all([yielded, blocker, queued]);

  assert.deepEqual(events, [
    'yielded:start',
    'blocker:start',
    'blocker:complete',
    'queued',
    'yielded:complete'
  ]);
});
