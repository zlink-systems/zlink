// SPDX-License-Identifier: MPL-2.0

'use strict';

process.env.ZLINK_NODE_TEST_HOOKS = '1';

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

interface CompletionEvent {
  token: bigint;
  result: number;
  terminalErrno: number;
}

interface NativeTestHooks {
  testSendCompletionOperationPath(
    mode: number,
    handler?: (event: CompletionEvent) => void
  ): CompletionEvent | undefined;
}

const addonPath = path.resolve(__dirname, '../../build/Release/zlink.node');
const native = require(addonPath) as NativeTestHooks;
const expectedKeys = ['result', 'terminalErrno', 'token'];

function nextTurn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

async function waitForCompletion(events: CompletionEvent[]): Promise<void> {
  for (let turn = 0; turn < 20 && events.length === 0; turn += 1) {
    await nextTurn();
  }
}

test('send completion handoff owns every terminal path exactly once', async () => {
  const direct = native.testSendCompletionOperationPath(0);
  assert.deepEqual(Object.keys(direct ?? {}).sort(), expectedKeys);
  assert.equal(direct?.token, 42n);
  assert.equal(direct?.result, 0);

  const early = native.testSendCompletionOperationPath(1);
  assert.deepEqual(Object.keys(early ?? {}).sort(), expectedKeys);
  assert.equal(early?.token, 42n);
  assert.equal(early?.result, 0);

  const queued: CompletionEvent[] = [];
  native.testSendCompletionOperationPath(2, (event) => queued.push(event));
  await waitForCompletion(queued);
  assert.equal(queued.length, 1);
  assert.deepEqual(Object.keys(queued[0]).sort(), expectedKeys);
  assert.equal(queued[0].token, 42n);
  assert.equal(queued[0].result, 0);

  const rejected: CompletionEvent[] = [];
  native.testSendCompletionOperationPath(3, (event) => rejected.push(event));
  await waitForCompletion(rejected);
  assert.equal(rejected.length, 1,
    'the queue-filling sentinel must still be delivered');
  assert.equal(rejected[0].token, 41n);
  assert.equal(rejected.some((event) => event.token === 42n), false,
    'the operation rejected by the full TSFN queue must not be delivered');
});
