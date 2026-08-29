import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ZLinkExecutionBarrier } from '../../packages/framework/src/runtime/execution';
import {
  ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS,
  ZLinkSerialExecutionQueue,
  type ZLinkSerialSchedulerOptions,
  type ZLinkSerialWorkRecord
} from '../../packages/framework/src/runtime/execution/serial-execution-queue';
import { ZLinkSpotSerialTurnExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-turn-executor';

interface Deferred<T> {
  readonly promise: Promise<T>;
  resolve(value: T): void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(complete => { resolve = complete; });
  return { promise, resolve };
}

function scheduler(
  sequences: bigint[] = [],
  options: ZLinkSerialSchedulerOptions = {}
): ZLinkSerialExecutionQueue {
  return new ZLinkSerialExecutionQueue(async (record: ZLinkSerialWorkRecord<unknown>) => {
    sequences.push(record.acceptedSequence);
    try {
      record.resolve(await record.operation());
    } catch (error) {
      record.reject(error);
    } finally {
      record.release();
      record.release();
    }
  }, options);
}

test('serial defaults include bounded owner-local reservations', () => {
  assert.equal(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS.ownerTimeBudget, 10);
  assert.equal(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS.lifecycleBurstLimit, 8);
  assert.ok(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS.applicationMessageCapacity > 0);
  assert.ok(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS.applicationByteCapacity > 0);
  assert.ok(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS.lifecycleMessageCapacity > 0);
  assert.ok(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS.lifecycleByteCapacity > 0);
  assert.ok(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS.fixedWorkByteCost > 0);
});

test('owner-local reservations survive terminal work and do not block another owner', async () => {
  const options = {
    applicationMessageCapacity: 1,
    applicationByteCapacity: 256,
    lifecycleMessageCapacity: 1,
    lifecycleByteCapacity: 256,
    ownerTimeBudget: 0,
    lifecycleBurstLimit: 1,
    fixedWorkByteCost: 64
  } as const;
  const serial = scheduler([], options);
  const otherOwner = scheduler([], options);
  const started = deferred<void>();
  const terminal = deferred<void>();
  const first = serial.submit(async () => {
    started.resolve();
    await terminal.promise;
  }, { payloadBytes: 128 });
  await started.promise;
  assert.deepEqual(serial.snapshot(), {
    applicationMessages: 1,
    applicationBytes: 192,
    lifecycleMessages: 0,
    lifecycleBytes: 0
  });
  await assert.rejects(serial.submit(() => undefined), /queue is full/u);
  assert.equal(await otherOwner.submit(() => 'other'), 'other');
  terminal.resolve();
  await first;
  assert.deepEqual(serial.snapshot(), {
    applicationMessages: 0,
    applicationBytes: 0,
    lifecycleMessages: 0,
    lifecycleBytes: 0
  });
  assert.equal(await serial.submit(() => 'following'), 'following');
});

test('a transferred record is accounted without capacity rejection until terminal completion', async () => {
  const serial = scheduler([], {
    applicationMessageCapacity: 1,
    applicationByteCapacity: 128,
    lifecycleMessageCapacity: 1,
    lifecycleByteCapacity: 128,
    ownerTimeBudget: 0,
    lifecycleBurstLimit: 1,
    fixedWorkByteCost: 32
  });
  const localStarted = deferred<void>();
  const localTerminal = deferred<void>();
  const transferredStarted = deferred<void>();
  const transferredTerminal = deferred<void>();
  const local = serial.submit(async () => {
    localStarted.resolve();
    await localTerminal.promise;
  }, { payloadBytes: 32, metadataBytes: 16 });
  await localStarted.promise;

  const transferred = serial.submitPreAdmitted(async () => {
    transferredStarted.resolve();
    await transferredTerminal.promise;
  }, { payloadBytes: 48, metadataBytes: 16 });
  assert.deepEqual(serial.snapshot(), {
    applicationMessages: 2,
    applicationBytes: 176,
    lifecycleMessages: 0,
    lifecycleBytes: 0
  });
  await assert.rejects(serial.submit(() => undefined), /queue is full/u);

  localTerminal.resolve();
  await local;
  await transferredStarted.promise;
  assert.deepEqual(serial.snapshot(), {
    applicationMessages: 1,
    applicationBytes: 96,
    lifecycleMessages: 0,
    lifecycleBytes: 0
  });
  transferredTerminal.resolve();
  await transferred;
  assert.deepEqual(serial.snapshot(), {
    applicationMessages: 0,
    applicationBytes: 0,
    lifecycleMessages: 0,
    lifecycleBytes: 0
  });
});

test('durable readiness yields to lifecycle arbitration without claiming the serial owner', async () => {
  const serial = scheduler([], { ownerTimeBudget: 0, lifecycleBurstLimit: 8 });
  const firstPreparationStarted = deferred<void>();
  const events: string[] = [];
  let attempts = 0;
  const durable = serial.admitDurablePrefix(
    () => { events.push('durable'); },
    {},
    undefined,
    {
      async prepare(signal) {
        attempts += 1;
        events.push(`prepare-${attempts}`);
        if (attempts !== 1) return;
        firstPreparationStarted.resolve();
        await new Promise<never>((_resolve, reject) => {
          const aborted = () => {
            events.push('prepare-aborted');
            reject(signal.reason);
          };
          signal.addEventListener('abort', aborted, { once: true });
        });
      },
      cancel() {
        events.push('preparation-canceled');
      }
    }
  );
  await firstPreparationStarted.promise;

  const lifecycle = serial.submit(
    () => { events.push('lifecycle'); },
    { lane: 'lifecycle' }
  );
  await Promise.all([lifecycle, durable]);

  assert.deepEqual(events, [
    'prepare-1',
    'prepare-aborted',
    'preparation-canceled',
    'lifecycle',
    'prepare-2',
    'durable',
    'preparation-canceled'
  ]);
});

test('a yielded Spot owner retains its reservation until the actual owner terminal', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(true, undefined, {
    applicationMessageCapacity: 1,
    applicationByteCapacity: 256,
    lifecycleMessageCapacity: 2,
    lifecycleByteCapacity: 256,
    ownerTimeBudget: 0,
    lifecycleBurstLimit: 1,
    fixedWorkByteCost: 64
  });
  const entered = deferred<void>();
  const resume = deferred<void>();
  const owner = serial.post(async () => {
    entered.resolve();
    await serial.yieldPromise(resume.promise);
  });
  await entered.promise;
  await assert.rejects(
    serial.post(() => undefined),
    /Spot execution queue capacity was exceeded/u
  );
  resume.resolve();
  await owner;
  let progressed = false;
  await serial.post(() => { progressed = true; });
  assert.equal(progressed, true);
});

test('terminal completion permits following application progress without framework admission', async () => {
  const serial = scheduler();
  const started = deferred<void>();
  const terminal = deferred<void>();
  const events: string[] = [];
  const first = serial.submit(async () => {
    events.push('first-start');
    started.resolve();
    await terminal.promise;
    events.push('first-terminal');
  });
  await started.promise;
  const queued = Array.from({ length: 32 }, (_, index) => serial.submit(() => {
    events.push(`queued-${index}`);
  }));
  terminal.resolve();
  await Promise.all([first, ...queued]);
  const following = serial.submit(() => events.push('following'));
  await following;
  assert.equal(events.at(-1), 'following');
  assert.equal(events.filter(value => value === 'first-terminal').length, 1);
});

test('application and lifecycle lanes preserve FIFO order', async () => {
  const serial = scheduler();
  const selected: string[] = [];
  await Promise.all([
    serial.submit(() => selected.push('application-a'), { lane: 'application' }),
    serial.submit(() => selected.push('application-b'), { lane: 'application' }),
    serial.submit(() => selected.push('lifecycle-a'), { lane: 'lifecycle' }),
    serial.submit(() => selected.push('lifecycle-b'), { lane: 'lifecycle' })
  ]);
  assert.deepEqual(selected.filter(value => value.startsWith('application')), [
    'application-a', 'application-b'
  ]);
  assert.deepEqual(selected.filter(value => value.startsWith('lifecycle')), [
    'lifecycle-a', 'lifecycle-b'
  ]);
});

test('an empty scheduler schedules new work without polling', async () => {
  const serial = scheduler();
  const observed: string[] = [];
  const work = serial.submit(() => observed.push('run'));
  assert.deepEqual(observed, []);
  await work;
  assert.deepEqual(observed, ['run']);
});

test('accepted sequences stay monotonic across terminal records', async () => {
  const sequences: bigint[] = [];
  const serial = scheduler(sequences);
  await Promise.all([
    serial.submit(() => undefined),
    serial.submit(() => undefined),
    serial.submit(() => undefined)
  ]);
  assert.deepEqual(sequences, [1n, 2n, 3n]);
});

test('execution barriers quiesce after queued terminal work', async () => {
  const barrier = new ZLinkExecutionBarrier();
  const serial = new ZLinkSpotSerialTurnExecutor(true);
  serial.setExecutionBarrier(barrier);
  await serial.post(() => undefined);
  const seal = barrier.seal();
  await barrier.waitForQuiescence(seal);
  assert.equal(barrier.abort(seal), true);
});

test('Spot one-way turns report the terminal handler error and continue', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(false);
  const reported = deferred<unknown>();
  await serial.postOneWay(
    () => { throw new Error('terminal'); },
    error => reported.resolve(error)
  );
  const error = await reported.promise;
  assert.equal(error instanceof Error && error.message, 'terminal');
  let progressed = false;
  await serial.post(() => { progressed = true; });
  assert.equal(progressed, true);
});
