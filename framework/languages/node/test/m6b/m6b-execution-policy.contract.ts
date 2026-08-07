import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  ZLinkUserSpotExecutionMode
} from '../../packages/framework/src/contracts/Configuration/ObjectRoles';
import { ZLinkConfigurationException } from '../../packages/framework/src/runtime/configuration';
import { ZLinkSpotActivation } from '../../packages/framework/src/runtime/spots/spot-activation-state';
import { ZLinkSpotSerialExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-executor';
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
  serial: ZLinkSpotSerialExecutor,
  executionMode: ZLinkUserSpotExecutionMode
): ZLinkSpotActivation {
  return new ZLinkSpotActivation({
    meshName: 'mesh',
    spotId: 'spot-1' as never,
    spotType: class TestSpot {} as never,
    spot: {} as never,
    serial,
    executionMode,
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

test('SpotWide Yield releases the Spot gate but retains the Actor claim', async () => {
  const serial = new ZLinkSpotSerialExecutor(true);
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
  const spotSerial = new ZLinkSpotSerialExecutor(false);
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
    new ZLinkSpotSerialExecutor(),
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

  const perActorSerial = new ZLinkSpotSerialExecutor(false);
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
  const spotSerial = new ZLinkSpotSerialExecutor(false);
  const timerSerials = new Map<string, ZLinkSpotSerialExecutor>();
  const registry = new ZLinkSpotTimerRegistry(
    undefined,
    () => false,
    (name) => {
      let serial = timerSerials.get(name);
      if (serial === undefined) {
        serial = new ZLinkSpotSerialExecutor(false);
        timerSerials.set(name, serial);
      }
      return serial;
    }
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

  assert.notEqual(timerSerials.get('heartbeat'), timerSerials.get('expiry'));
  assert.notEqual(timerSerials.get('heartbeat'), spotSerial);
  assert.notEqual(timerSerials.get('expiry'), spotSerial);
  await registry.dispose();
});

test('Spot execution reserves message and byte capacity as one bounded admission', async () => {
  const serial = new ZLinkSpotSerialExecutor(false, undefined, {
    applicationMessageCapacity: 4,
    applicationByteCapacity: 600,
    lifecycleMessageCapacity: 2,
    lifecycleByteCapacity: 512,
    ownerTimeBudgetMs: 50,
    lifecycleBurstLimit: 8,
    fixedWorkByteCost: 256
  });
  const startedSignal = deferred<void>();
  const finished = deferred<void>();
  const first = serial.execute(async () => {
    startedSignal.resolve();
    await finished.promise;
  });
  await startedSignal.promise;

  const accepted = serial.execute(() => undefined);
  await assert.rejects(
    () => serial.execute(() => undefined, { payloadBytes: 100 }),
    (error: unknown) => error instanceof Error && 'kind' in error && (error as { kind?: number }).kind === 6
  );
  finished.resolve();
  await Promise.all([first, accepted]);
});

test('Spot execution includes metadata bytes in the same atomic reservation', async () => {
  const serial = new ZLinkSpotSerialExecutor(false, undefined, {
    applicationMessageCapacity: 2,
    applicationByteCapacity: 399,
    lifecycleMessageCapacity: 1,
    lifecycleByteCapacity: 256,
    fixedWorkByteCost: 256
  });
  const started = deferred<void>();
  const finished = deferred<void>();
  const first = serial.execute(async () => {
    started.resolve();
    await finished.promise;
  });
  await started.promise;

  await assert.rejects(
    () => serial.execute(() => undefined, { payloadBytes: 100, metadataBytes: 50 }),
    (error: unknown) => error instanceof Error && 'kind' in error && (error as { kind?: number }).kind === 6
  );
  finished.resolve();
  await first;
});

test('Spot barrier turns remain in application FIFO order and scheduler yields at its time budget', async () => {
  const serial = new ZLinkSpotSerialExecutor(false, undefined, {
    applicationMessageCapacity: 256,
    applicationByteCapacity: 256 * 256,
    lifecycleMessageCapacity: 16,
    lifecycleByteCapacity: 16 * 256,
    ownerTimeBudgetMs: 1,
    lifecycleBurstLimit: 8,
    fixedWorkByteCost: 256
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
  const serial = new ZLinkSpotSerialExecutor(true);
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
