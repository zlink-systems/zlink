const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkUserSpotExecutionMode } = require('../../packages/framework/dist/contracts');
const {
  ZLinkSpotSerialExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-executor');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  ZLinkSerialExecutionQueue
} = require('../../packages/framework/dist/runtime/execution/serial-execution-queue');

function deferred() {
  let resolve;
  const promise = new Promise((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

test('a mailbox control failure retains its original cause', async () => {
  const owner = new ZLinkSerialExecutionQueue(async () => {});
  const mailbox = new ZLinkSerialExecutionQueue(async () => {}, {}, owner);
  const failure = new Error('mailbox control failed');
  const drain = owner.drain.bind(owner);
  let observed;
  owner.drain = async () => {
    try {
      await drain();
    } catch (error) {
      observed = error;
    }
  };
  mailbox.enqueueMailboxTurn(() => {
    throw failure;
  });
  await new Promise(setImmediate);
  assert.equal(observed, failure);
});

async function assertConsumerOwnership(injectIndependentConsumer = false) {
  const spot = new ZLinkSpotSerialTurnExecutor();
  const executor = new ZLinkSpotSerialExecutor(spot, ZLinkUserSpotExecutionMode.SpotWide, 'spot');
  const started = deferred();
  const release = deferred();
  const occupied = executor.executeSpot(async () => {
    started.resolve();
    await release.promise;
  });
  await started.promise;
  const dequeues = [];
  const actors = ['a', 'b'].map((id) =>
    executor.executeActor(id, (serial) => serial.execute(() => id))
  );
  for (const [id, actor] of executor.actorExecutors) {
    const queue = actor.scheduler;
    const take = queue.takeSelected;
    queue.takeSelected = function (selection) {
      dequeues.push({ id, sequence: selection.record.acceptedSequence });
      return take.call(this, selection);
    };
  }
  try {
    if (injectIndependentConsumer) {
      // Deliberately restore the old independent consumer to verify detection.
      for (const actor of executor.actorExecutors.values()) {
        const queue = actor.scheduler;
        // The old consumer selected and claimed the head before submitting its Spot turn.
        const selection = queue.selectNext();
        const record = queue.takeSelected(selection);
        void queue.executeRecord(record);
      }
    }
    await new Promise(setImmediate);
    assert.deepEqual(
      dequeues,
      [],
      'producer admission must not start an independent Actor consumer'
    );
    for (const actor of executor.actorExecutors.values()) {
      assert.equal(actor.scheduler.application.records.length, 1);
    }
  } finally {
    release.resolve();
    await Promise.all([occupied, ...actors]);
  }
  assert.deepEqual(dequeues, [
    { id: 'a', sequence: 1n },
    { id: 'b', sequence: 1n }
  ]);
  await executor.close();
}

test('SpotWide defers Actor dequeue until the occupied shared gate is released', async () => {
  await assertConsumerOwnership();
});

test('the ownership observation rejects an injected independent Actor consumer', async () => {
  await assert.rejects(assertConsumerOwnership(true), {
    name: 'AssertionError',
    message: /producer admission must not start an independent Actor consumer/
  });
});

test('SpotWide returns Actor terminal settlement and next-head selection to the shared gate', async () => {
  const spot = new ZLinkSpotSerialTurnExecutor();
  const executor = new ZLinkSpotSerialExecutor(spot, ZLinkUserSpotExecutionMode.SpotWide, 'spot');
  const started = deferred();
  const terminal = deferred();
  let settled = false;
  const first = executor
    .executeActor('a', () => {
      started.resolve();
      return terminal.promise;
    })
    .then(() => {
      settled = true;
    });
  await started.promise;
  const blockerStarted = deferred();
  const release = deferred();
  const blocker = executor.executeSpot(async () => {
    blockerStarted.resolve();
    await release.promise;
  });
  await blockerStarted.promise;
  let secondStarted = false;
  const second = executor.executeActor('a', () => {
    secondStarted = true;
  });
  try {
    terminal.resolve();
    await new Promise(setImmediate);
    assert.equal(settled, false);
    assert.equal(secondStarted, false);
    assert.equal(executor.actorExecutors.get('a').scheduler.application.records.length, 1);
  } finally {
    release.resolve();
    await Promise.all([first, second, blocker]);
  }
  assert.equal(settled, true);
  assert.equal(secondStarted, true);
  await executor.close();
});

test('SpotWide Yield keeps the Actor claim while another Actor and a timer progress', async () => {
  const spot = new ZLinkSpotSerialTurnExecutor();
  const executor = new ZLinkSpotSerialExecutor(spot, ZLinkUserSpotExecutionMode.SpotWide, 'spot');
  const pending = deferred();
  const yielded = deferred();
  const events = [];
  const first = executor.executeActor('a', (serial) =>
    serial.execute(async () => {
      events.push('a1:start');
      const result = serial.yieldPromise(pending.promise);
      yielded.resolve();
      await result;
      events.push('a1:end');
    })
  );
  await yielded.promise;
  const second = executor.executeActor('a', (serial) => serial.execute(() => events.push('a2')));
  await Promise.all([
    executor.executeActor('b', (serial) => serial.execute(() => events.push('b'))),
    executor.executeTimer('tick', () => events.push('timer'))
  ]);
  assert.equal(events.includes('a2'), false);
  pending.resolve();
  await Promise.all([first, second]);
  assert.ok(events.indexOf('a2') > events.indexOf('a1:end'));
  await executor.close();
});

test('passive Actor lanes retain fairness and synchronous acceptance sequence', async () => {
  const spot = new ZLinkSpotSerialTurnExecutor();
  const executor = new ZLinkSpotSerialExecutor(spot, ZLinkUserSpotExecutionMode.SpotWide, 'spot', {
    lifecycleBurstLimit: 2
  });
  const events = [];
  const admitted = [
    executor.executeActor('a', () => events.push('app')),
    ...[1, 2, 3].map((id) =>
      executor.executeActor('a', () => events.push(`life${id}`), { lane: 'lifecycle' })
    )
  ];
  const mailbox = executor.actorExecutors.get('a').scheduler;
  assert.deepEqual(
    mailbox.application.records.map((r) => r.acceptedSequence),
    [1n]
  );
  assert.deepEqual(
    mailbox.lifecycle.records.map((r) => r.acceptedSequence),
    [2n, 3n, 4n]
  );
  await Promise.all(admitted);
  assert.deepEqual(events, ['life1', 'life2', 'app', 'life3']);
  await executor.close();
});
