const assert = require('node:assert/strict');
const { createHook } = require('node:async_hooks');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const {
  RuntimeEventQueue,
  saturatingObservationLossIncrement,
  ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
  ZLINK_SIGNED_OBSERVATION_LOSS_MAXIMUM
} = require('../../packages/framework/dist/runtime/diagnostics/runtime-observation-queue');

const fixturePath = path.resolve(
  __dirname,
  '../../../../runtime/conformance/runtime-observation-v1.json'
);
const fixture = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));

test('runtime observation consumes the canonical multi-source retention fixture', async () => {
  const scenario = fixture.scenarios.find(
    candidate => candidate.name === 'multi-source-retention-and-terminal-overflow'
  );
  assert.ok(scenario);
  const queue = new RuntimeEventQueue(scenario.terminalCapacity);

  for (const operation of scenario.operations) {
    if (operation.kind === 'terminal') {
      queue.pushTerminal(operation, operation.source);
    } else {
      queue.push(operation, operation.source);
    }
  }

  const retainedCount = scenario.expectedTerminalFifo.length
    + Object.keys(scenario.expectedRetainedIntermediateBySource).length;
  const retained = [];
  for (let index = 0; index < retainedCount; index += 1) {
    const observed = await queue.next();
    assert.equal(observed.done, false);
    retained.push(observed.value);
  }

  const terminals = retained
    .filter(observed => observed.status.kind === 'terminal')
    .map(observed => ({
      source: observed.status.source,
      sequence: observed.status.sequence,
      value: observed.status.value
    }));
  assert.deepEqual(terminals, scenario.expectedTerminalFifo);

  const intermediates = Object.fromEntries(retained
    .filter(observed => observed.status.kind === 'intermediate')
    .map(observed => [observed.status.source, {
      sequence: observed.status.sequence,
      value: observed.status.value
    }]));
  assert.deepEqual(intermediates, scenario.expectedRetainedIntermediateBySource);

  const retainedSources = new Set(retained.map(observed => observed.status.source));
  assert.deepEqual(
    [...retainedSources].sort(),
    [...scenario.expectedRetainedSourceKeys].sort()
  );
  for (const source of scenario.expectedRemovedSourceKeys) {
    assert.equal(retainedSources.has(source), false);
  }

  const expectedLoss = {
    coalescedCount: BigInt(scenario.expectedLoss.coalescedIntermediateCount),
    discardedTerminalCount: BigInt(scenario.expectedLoss.discardedTerminalCount)
  };
  for (const observed of retained) assert.deepEqual(observed.loss, expectedLoss);

  const next = queue.next();
  queue.push({ kind: 'intermediate', source: 'E', sequence: 1, value: 'E1' }, 'E');
  assert.equal((await next).value.status.source, 'E');
  await queue.return();
});

test('default FIFO64 discards the oldest terminal without closing the subscriber', async () => {
  assert.equal(
    ZLINK_DEFAULT_TERMINAL_OBSERVATION_CAPACITY,
    fixture.limits.defaultTerminalCapacity
  );
  const queue = new RuntimeEventQueue();
  for (let index = 0; index <= fixture.limits.defaultTerminalCapacity; index += 1) {
    queue.pushTerminal({ source: `source-${index}`, sequence: 1 }, `source-${index}`);
  }

  const oldestRetained = await queue.next();
  assert.equal(oldestRetained.value.status.source, 'source-1');
  assert.deepEqual(oldestRetained.value.loss, {
    coalescedCount: 0n,
    discardedTerminalCount: 1n
  });

  const stillOpen = queue.next();
  queue.push({ source: 'live-source', sequence: 1 }, 'live-source');
  assert.equal((await stillOpen).done, false);
  await queue.return();
});

test('terminal delivery and discard release source-key lifetime independently', async () => {
  const queue = new RuntimeEventQueue(1);
  queue.pushTerminal({ source: 'A', sequence: 2 }, 'A');
  queue.push({ source: 'A', sequence: 3 }, 'A');
  queue.pushTerminal({ source: 'B', sequence: 1 }, 'B');

  // A can begin a new source lifetime after its terminal was discarded.
  queue.push({ source: 'A', sequence: 1 }, 'A');
  // B cannot restart while its terminal still owns the source key.
  queue.push({ source: 'B', sequence: 2 }, 'B');
  const retained = [(await queue.next()).value, (await queue.next()).value];
  assert.deepEqual(
    retained.map(observed => observed.status).sort((left, right) =>
      left.source.localeCompare(right.source)),
    [{ source: 'A', sequence: 1 }, { source: 'B', sequence: 1 }]
  );
  for (const observed of retained) {
    assert.deepEqual(observed.loss, {
      coalescedCount: 2n,
      discardedTerminalCount: 1n
    });
  }

  // B can likewise begin a new source lifetime after its terminal was delivered.
  queue.push({ source: 'B', sequence: 1 }, 'B');
  const restartedB = await queue.next();
  assert.deepEqual(restartedB.value.status, { source: 'B', sequence: 1 });
  assert.deepEqual(restartedB.value.loss, {
    coalescedCount: 2n,
    discardedTerminalCount: 1n
  });
  await queue.return();
});

test('sealed terminal streams remain subscriber-abortable after source detachment', async () => {
  const controller = new AbortController();
  const queue = new RuntimeEventQueue(1, controller.signal);
  let sourceDetached = false;
  queue.onClose(() => { sourceDetached = true; });
  queue.seal({ source: 'A', sequence: 1 }, 'A');
  assert.equal(sourceDetached, true);
  assert.equal((await queue.next()).done, false);

  const completion = queue.next();
  controller.abort();
  assert.equal((await completion).done, true);
});

test('canonical signed loss counters saturate independently and restart at zero', async () => {
  const scenario = fixture.scenarios.find(
    candidate => candidate.name === 'loss-counters-saturate-independently'
  );
  assert.ok(scenario);
  assert.equal(
    ZLINK_SIGNED_OBSERVATION_LOSS_MAXIMUM,
    BigInt(fixture.limits.signedLossCounterMaximum)
  );
  let coalescedCount = BigInt(scenario.initialLoss.coalescedIntermediateCount);
  let discardedTerminalCount = BigInt(
    scenario.initialLoss.discardedTerminalCount
  );

  for (let index = 0; index < scenario.increments.coalescedIntermediateCount; index += 1) {
    coalescedCount = saturatingObservationLossIncrement(coalescedCount);
  }
  assert.equal(
    discardedTerminalCount,
    BigInt(scenario.initialLoss.discardedTerminalCount)
  );
  for (let index = 0; index < scenario.increments.discardedTerminalCount; index += 1) {
    discardedTerminalCount = saturatingObservationLossIncrement(discardedTerminalCount);
  }
  assert.deepEqual({ coalescedCount, discardedTerminalCount }, {
    coalescedCount: BigInt(scenario.expectedLoss.coalescedIntermediateCount),
    discardedTerminalCount: BigInt(scenario.expectedLoss.discardedTerminalCount)
  });

  const newSubscriber = new RuntimeEventQueue();
  newSubscriber.push({ sequence: 1 });
  assert.deepEqual((await newSubscriber.next()).value.loss, {
    coalescedCount: 0n,
    discardedTerminalCount: 0n
  });
  await newSubscriber.return();
});

test('slow observers share one dispatcher and producers do not resolve subscribers inline', async () => {
  const slow = new RuntimeEventQueue();
  const fast = new RuntimeEventQueue();
  const originalQueueMicrotask = global.queueMicrotask;
  let scheduledMicrotasks = 0;
  let publishing = false;
  let promiseResolvedWhilePublishing = false;
  const hook = createHook({
    promiseResolve() {
      if (publishing) promiseResolvedWhilePublishing = true;
    }
  });
  global.queueMicrotask = callback => {
    scheduledMicrotasks += 1;
    originalQueueMicrotask(callback);
  };
  hook.enable();

  let releaseSlow;
  const slowGate = new Promise(resolve => { releaseSlow = resolve; });
  let slowFinished = false;
  try {
    const slowConsumer = slow.next().then(async observed => {
      await slowGate;
      slowFinished = true;
      return observed;
    });
    const fastConsumer = fast.next();

    publishing = true;
    slow.push({ source: 'slow', sequence: 1 }, 'slow');
    fast.push({ source: 'fast', sequence: 1 }, 'fast');
    publishing = false;

    assert.equal(promiseResolvedWhilePublishing, false);
    assert.equal(scheduledMicrotasks, 1);
    assert.equal((await fastConsumer).value.status.source, 'fast');
    assert.equal(slowFinished, false);
    releaseSlow();
    assert.equal((await slowConsumer).value.status.source, 'slow');
  } finally {
    publishing = false;
    hook.disable();
    global.queueMicrotask = originalQueueMicrotask;
    releaseSlow?.();
    await slow.return();
    await fast.return();
  }
});
