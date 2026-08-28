import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ZLinkUserSpotExecutionMode } from '../../packages/framework/src/contracts/Configuration/ObjectRoles';
import { ZLinkSpotSerialExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-executor';
import { ZLinkSpotSerialTurnExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-turn-executor';
import type { ZLinkSerialSchedulerOptions } from '../../packages/framework/src/runtime/execution/serial-execution-queue';

interface Deferred<T> {
  readonly promise: Promise<T>;
  resolve(value: T): void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(complete => { resolve = complete; });
  return { promise, resolve };
}

function executor(
  mode: ZLinkUserSpotExecutionMode,
  spotOptions?: ZLinkSerialSchedulerOptions,
  actorOptions?: ZLinkSerialSchedulerOptions
): ZLinkSpotSerialExecutor {
  return new ZLinkSpotSerialExecutor(
    new ZLinkSpotSerialTurnExecutor(mode === ZLinkUserSpotExecutionMode.SpotWide, undefined, spotOptions),
    mode,
    'spot-1',
    actorOptions
  );
}

function runActor<T>(
  serialExecutor: ZLinkSpotSerialExecutor,
  actorId: string,
  operation: () => Promise<T> | T,
  payloadBytes?: number
): Promise<T> {
  return serialExecutor.executeActor(
    actorId,
    serial => serial.execute(operation),
    payloadBytes === undefined ? undefined : { payloadBytes }
  );
}

function blockFor(ms: number): void {
  const deadline = performance.now() + ms;
  while (performance.now() < deadline) {
    // Keep this synchronous work inside the submitted handler turn.
  }
}

test('serial executor submission paths select queues without an application queue argument', async () => {
  const serialExecutor = executor(ZLinkUserSpotExecutionMode.PerActor);

  assert.equal(await serialExecutor.executeSpot(() => 'spot'), 'spot');
  assert.equal(await runActor(serialExecutor, 'actor-a', () => 'actor'), 'actor');
  assert.equal(await serialExecutor.executeTimer('tick', () => 'timer'), 'timer');
  assert.equal(await serialExecutor.executeLifecycle(() => 'lifecycle'), 'lifecycle');
});

test('PerActor runs two Actor handlers concurrently across await boundaries', async () => {
  const serialExecutor = executor(ZLinkUserSpotExecutionMode.PerActor);
  const release = deferred<void>();
  const bothStarted = deferred<void>();
  const events: string[] = [];

  const first = runActor(serialExecutor, 'actor-a', async () => {
    events.push('actor-a:start');
    if (events.includes('actor-b:start')) bothStarted.resolve();
    await release.promise;
    events.push('actor-a:end');
  });
  const second = runActor(serialExecutor, 'actor-b', async () => {
    events.push('actor-b:start');
    if (events.includes('actor-a:start')) bothStarted.resolve();
    await release.promise;
    events.push('actor-b:end');
  });

  await bothStarted.promise;
  assert.deepEqual(events.sort(), ['actor-a:start', 'actor-b:start']);
  release.resolve();
  await Promise.all([first, second]);
});

test('SpotWide starts the next Actor handler only after the preceding handler finishes', async () => {
  const serialExecutor = executor(ZLinkUserSpotExecutionMode.SpotWide);
  const release = deferred<void>();
  const firstStarted = deferred<void>();
  const events: string[] = [];

  const first = runActor(serialExecutor, 'actor-a', async () => {
    events.push('actor-a:start');
    firstStarted.resolve();
    await release.promise;
    events.push('actor-a:end');
  });
  await firstStarted.promise;
  const second = runActor(serialExecutor, 'actor-b', () => events.push('actor-b:start'));
  await Promise.resolve();
  assert.deepEqual(events, ['actor-a:start']);
  release.resolve();
  await Promise.all([first, second]);
  assert.deepEqual(events, ['actor-a:start', 'actor-a:end', 'actor-b:start']);
});

test('the same Actor retains submission order in both execution modes', async () => {
  for (const mode of [
    ZLinkUserSpotExecutionMode.PerActor,
    ZLinkUserSpotExecutionMode.SpotWide
  ]) {
    const serialExecutor = executor(mode);
    const events: string[] = [];
    await Promise.all([
      runActor(serialExecutor, 'actor-a', () => events.push('first')),
      runActor(serialExecutor, 'actor-a', () => events.push('second'))
    ]);
    assert.deepEqual(events, ['first', 'second']);
  }
});

test('PerActor timer names overlap while one timer name retains submission order', async () => {
  const serialExecutor = executor(ZLinkUserSpotExecutionMode.PerActor);
  const release = deferred<void>();
  const tickStarted = deferred<void>();
  const beatStarted = deferred<void>();
  const events: string[] = [];

  const tickFirst = serialExecutor.executeTimer('tick', async () => {
    events.push('tick:first:start');
    tickStarted.resolve();
    await release.promise;
    events.push('tick:first:end');
  });
  await tickStarted.promise;
  const tickSecond = serialExecutor.executeTimer('tick', () => events.push('tick:second'));
  const beat = serialExecutor.executeTimer('beat', () => {
    events.push('beat');
    beatStarted.resolve();
  });
  await beatStarted.promise;
  assert.deepEqual(events, ['tick:first:start', 'beat']);
  release.resolve();
  await Promise.all([tickFirst, tickSecond, beat]);
  assert.deepEqual(events, ['tick:first:start', 'beat', 'tick:first:end', 'tick:second']);
});

test('Actor mailbox capacity rejects only the full Actor and accepts another Actor', async () => {
  const serialExecutor = executor(
    ZLinkUserSpotExecutionMode.PerActor,
    undefined,
    {
      applicationMessageCapacity: 1,
      applicationByteCapacity: 1024,
      lifecycleMessageCapacity: 1,
      lifecycleByteCapacity: 1024,
      fixedWorkByteCost: 1
    }
  );
  const release = deferred<void>();
  const started = deferred<void>();
  const first = runActor(serialExecutor, 'actor-a', async () => {
    started.resolve();
    await release.promise;
  });
  await started.promise;

  await assert.rejects(runActor(serialExecutor, 'actor-a', () => undefined), /Actor execution queue capacity was exceeded/u);
  assert.equal(await runActor(serialExecutor, 'actor-b', () => 'accepted'), 'accepted');
  release.resolve();
  await first;
});

test('SpotWide Actor mailbox reserves large payloads before small payloads', async () => {
  const serialExecutor = executor(
    ZLinkUserSpotExecutionMode.SpotWide,
    undefined,
    {
      applicationMessageCapacity: 8,
      applicationByteCapacity: 100,
      lifecycleMessageCapacity: 1,
      lifecycleByteCapacity: 100,
      fixedWorkByteCost: 1
    }
  );
  const release = deferred<void>();
  const started = deferred<void>();
  const first = runActor(serialExecutor, 'actor-a', async () => {
    started.resolve();
    await release.promise;
  }, 80);
  await started.promise;

  await assert.rejects(runActor(serialExecutor, 'actor-a', () => undefined, 80), /Actor execution queue capacity was exceeded/u);
  const small = runActor(serialExecutor, 'actor-a', () => undefined, 10);
  release.resolve();
  await Promise.all([first, small]);
});

test('SpotWide upper queue capacity is reached by work count for both small and large Actor payloads', async () => {
  const rejectedAfterOne = async (payloadBytes: number): Promise<void> => {
    const serialExecutor = executor(
      ZLinkUserSpotExecutionMode.SpotWide,
      {
        applicationMessageCapacity: 1,
        applicationByteCapacity: 2,
        lifecycleMessageCapacity: 1,
        lifecycleByteCapacity: 2,
        fixedWorkByteCost: 1
      }
    );
    const release = deferred<void>();
    const started = deferred<void>();
    const first = runActor(serialExecutor, 'actor-a', async () => {
      started.resolve();
      await release.promise;
    }, 1);
    await started.promise;
    await assert.rejects(
      runActor(serialExecutor, 'actor-b', () => undefined, payloadBytes),
      /Spot execution queue capacity was exceeded/u
    );
    release.resolve();
    await first;
  };

  await rejectedAfterOne(1);
  await rejectedAfterOne(10_000);
});

test('lifecycle work overtakes application only up to lifecycleBurstLimit', async () => {
  const serialExecutor = executor(ZLinkUserSpotExecutionMode.SpotWide, {
    applicationMessageCapacity: 8,
    applicationByteCapacity: 1024,
    lifecycleMessageCapacity: 8,
    lifecycleByteCapacity: 1024,
    lifecycleBurstLimit: 2,
    fixedWorkByteCost: 1
  });
  const events: string[] = [];

  await Promise.all([
    serialExecutor.executeSpot(() => events.push('application')),
    serialExecutor.executeLifecycle(() => events.push('lifecycle-1')),
    serialExecutor.executeLifecycle(() => events.push('lifecycle-2')),
    serialExecutor.executeLifecycle(() => events.push('lifecycle-3'))
  ]);

  assert.deepEqual(events, ['lifecycle-1', 'lifecycle-2', 'application', 'lifecycle-3']);
});

test('owner time budget yields an overloaded Actor before its remaining records monopolize the event loop', async () => {
  const serialExecutor = executor(
    ZLinkUserSpotExecutionMode.PerActor,
    undefined,
    {
      applicationMessageCapacity: 32,
      applicationByteCapacity: 1024,
      lifecycleMessageCapacity: 1,
      lifecycleByteCapacity: 1024,
      ownerTimeBudget: 1,
      fixedWorkByteCost: 1
    }
  );
  const events: string[] = [];
  const overloaded = Array.from({ length: 8 }, (_, index) =>
    runActor(serialExecutor, 'actor-a', () => {
      events.push(`actor-a:${index}`);
      blockFor(2);
    })
  );
  const other = new Promise<void>((resolve, reject) => {
    setImmediate(() => {
      void runActor(serialExecutor, 'actor-b', () => events.push('actor-b'))
        .then(() => resolve(), reject);
    });
  });

  await Promise.all([...overloaded, other]);
  assert.ok(events.indexOf('actor-b') < events.lastIndexOf('actor-a:7'));
});

test('coordinator re-entry that synchronously waits on execute completion throws immediately', async () => {
  const serialExecutor = executor(ZLinkUserSpotExecutionMode.SpotWide);
  await serialExecutor.executeSpot(() => {
    assert.throws(() => serialExecutor.executeSpot(() => undefined), /cannot implicitly execute another turn/u);
  });
});

test('Node overlap is observable as two async Actor handlers alternating around await', async () => {
  const serialExecutor = executor(ZLinkUserSpotExecutionMode.PerActor);
  const release = deferred<void>();
  const bothStarted = deferred<void>();
  const events: string[] = [];

  const first = runActor(serialExecutor, 'actor-a', async () => {
    events.push('actor-a:before-await');
    if (events.includes('actor-b:before-await')) bothStarted.resolve();
    await release.promise;
    events.push('actor-a:after-await');
  });
  const second = runActor(serialExecutor, 'actor-b', async () => {
    events.push('actor-b:before-await');
    if (events.includes('actor-a:before-await')) bothStarted.resolve();
    await release.promise;
    events.push('actor-b:after-await');
  });

  await bothStarted.promise;
  assert.deepEqual(events.sort(), ['actor-a:before-await', 'actor-b:before-await']);
  release.resolve();
  await Promise.all([first, second]);
});
