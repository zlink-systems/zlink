// SPDX-License-Identifier: MPL-2.0
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
require('@zlink-systems/framework');
const {
  ApplicationJobQueue, resolveApplicationJobQueueConfiguration
} = require('../../../../languages/node/packages/framework/dist/runtime/host/application-job-queue');
const { createReceivePermitPump } = require('./receive-permit-pump');
const RECEIVE_RECORD_BUDGET = 64;

function fixture(max = 128n) {
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({
    maxQueuedApplicationJobs: max
  }));
  const stop = new AbortController();
  const errors = [];
  const calls = [];
  for (const name of ['createReceiveRotation', 'acquireOrResume']) {
    const original = queue[name].bind(queue);
    queue[name] = (...args) => {
      calls.push(name);
      const value = original(...args);
      if (name === 'createReceiveRotation') {
        for (const method of ['reserve', 'take', 'releaseUnused']) {
          const invoke = value[method].bind(value);
          value[method] = (...args) => { calls.push(method); return invoke(...args); };
        }
      }
      return value;
    };
  }
  return { queue, stop, errors, calls,
    pump: (receive, processMessage = (permit) => permit.releaseBeforeHandler()) =>
      createReceivePermitPump(queue, receive, processMessage,
        (error, phase) => errors.push({ error, phase }), stop.signal, RECEIVE_RECORD_BUDGET)
  };
}

test('rotation fast path is synchronous, bounded, and reuses permit storage', () => {
  const f = fixture();
  const permits = new Set();
  const pump = f.pump(() => true, (permit) => {
    permits.add(permit);
    permit.releaseBeforeHandler();
  });
  assert.equal(pump(), RECEIVE_RECORD_BUDGET);
  assert.equal(pump(), RECEIVE_RECORD_BUDGET);
  assert.equal(permits.size, 1);
  assert.equal(f.calls.filter((x) => x === 'createReceiveRotation').length, 1);
  assert.equal(f.calls.includes('acquireOrResume'), false);
  assert.deepEqual(f.calls.slice(0, 4), ['createReceiveRotation', 'reserve', 'releaseUnused', 'take']);
  assert.equal(f.calls.at(-1), 'releaseUnused');
  assert.equal(f.queue.snapshot().permitsInUse, 0n);
  assert.deepEqual(f.errors, []);
});

test('NoData returns the taken permit storage and the unconsumed rotation count', () => {
  const f = fixture();
  const pump = f.pump(() => false);
  assert.equal(pump(), 0);
  assert.equal(pump(), 0);
  assert.equal(f.calls.includes('acquireOrResume'), false);
  assert.equal(f.queue.snapshot().permitsInUse, 0n);
  assert.deepEqual(f.errors, []);
});

test('short capacity ends the round, registers one FIFO wait, and consumes its saved grant', async () => {
  const f = fixture(1n);
  const held = await f.queue.acquire(undefined, 'remote');
  f.calls.length = 0;
  let receives = 0;
  const pump = f.pump(() => ++receives === 1);
  assert.equal(pump(), 0);
  assert.equal(pump(), 0);
  assert.equal(receives, 0);
  assert.equal(f.calls.filter((x) => x === 'acquireOrResume').length, 1);
  assert.equal(f.queue.snapshot().capacityWaiters, 1n);
  held.releaseAfterInternalProcessing();
  await new Promise(setImmediate);
  assert.equal(f.queue.snapshot().capacityWaiters, 0n);
  assert.equal(pump(), 1);
  assert.equal(f.queue.snapshot().permitsInUse, 0n);
  assert.equal(f.queue.snapshot().capacityWaitCount, 1n);
  assert.deepEqual(f.errors, []);
});

test('receive and handler failures return both capacity and reusable storage', () => {
  for (const phase of ['recv', 'handler']) {
    const f = fixture();
    const failure = new Error(phase);
    let available = true;
    const pump = f.pump(() => {
      if (phase === 'recv') throw failure;
      const result = available;
      available = false;
      return result;
    }, () => { throw failure; });
    assert.equal(pump(), phase === 'recv' ? 0 : 1);
    assert.deepEqual(f.errors, [{ error: failure, phase }]);
    assert.equal(f.queue.snapshot().permitsInUse, 0n);
    assert.equal(f.calls.at(-1), 'releaseUnused');
  }
});

test('stopping a pending capacity wait removes the waiter without reporting a receive error', async () => {
  const f = fixture(1n);
  const held = await f.queue.acquire(undefined, 'remote');
  assert.equal(f.pump(() => true)(), 0);
  f.stop.abort();
  await new Promise(setImmediate);
  held.releaseAfterInternalProcessing();
  assert.equal(f.queue.snapshot().capacityWaiters, 0n);
  assert.equal(f.queue.snapshot().permitsInUse, 0n);
  assert.deepEqual(f.errors, []);
});


test('permit receive keeps the caller drain limit instead of imposing the runtime 64-record limit', () => {
  const f = fixture(512n);
  let calls = 0;
  const pump = createReceivePermitPump(f.queue, () => { calls++; return true; },
    permit => permit.releaseBeforeHandler(),
    (error, phase) => f.errors.push({error, phase}), f.stop.signal, 256);
  assert.equal(pump(), 256);
  assert.equal(calls, 256);
  assert.equal(f.queue.snapshot().permitsInUse, 0n);
  assert.deepEqual(f.errors, []);
});
