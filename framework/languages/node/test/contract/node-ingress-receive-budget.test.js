'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkChannelReceiveLoop } = require('../../packages/framework/dist/runtime/channels/channel-receive-loops');

const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');

for (const [bytes, expected] of [[1, 64], [2 * 1024 * 1024, 2], [4 * 1024 * 1024, 1]]) {
  test(`receive budget yields after ${expected} records of ${bytes} bytes without BigInt allocation`, async t => {
    let received = 0;
    let closed = 0;
    let conversions = 0;
    let readinessChecks = 0;
    const stop = new AbortController();
    const loop = new ZLinkChannelReceiveLoop(
      'budget',
      { recv() {
        received++;
        return { parts: [{ size: () => bytes, data() { throw new Error('size accounting must not materialize data'); } }],
          close() { closed++; } };
      } },
      { dispatch() { throw new Error('control records must not enter application dispatch'); } },
      undefined,
      () => true,
      { wait() { readinessChecks++; return true; }, dispose() {} },
      new ApplicationJobQueue(resolveApplicationJobQueueConfiguration())
    );
    const bigint = BigInt;
    t.mock.method(global, 'BigInt', value => { conversions++; return bigint(value); });
    t.mock.method(performance, 'now', () => 0);
    setImmediate(() => stop.abort());
    try {
      await loop.run(stop.signal);
      assert.equal(received, expected);
      assert.equal(closed, expected);
      assert.equal(conversions, 0);
      assert.equal(readinessChecks, 1);
    } finally {
      stop.abort();
      await loop.stop();
    }
  });
}
