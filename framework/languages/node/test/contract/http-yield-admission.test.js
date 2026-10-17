/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHttpClient } = require('../../packages/http-client/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  createIntegrationHttpExecutionScheduler
} = require('../../packages/framework/dist/nest-integration');

function deferred() {
  let resolve;
  const promise = new Promise((r) => {
    resolve = r;
  });
  return { promise, resolve };
}

test('HTTP turn seam invokes factories after admission and admits the next resumed turn', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(true, 'http-owner');
  const scheduler = createIntegrationHttpExecutionScheduler({});
  let calls = 0;
  let captured;
  try {
    await serial.execute(async () => {
      captured = scheduler.capture();
      const first = captured.yieldPromise(() => {
        calls++;
        return Promise.resolve('first');
      });
      assert.throws(
        () =>
          captured.yieldPromise(() => {
            calls++;
            return Promise.resolve('rejected');
          }),
        { kind: ZLinkFrameworkErrorKind.InvalidOperation }
      );
      assert.equal(await first, 'first');
      assert.equal(
        await captured.yieldPromise(() => {
          calls++;
          return Promise.resolve('resumed');
        }),
        'resumed'
      );
    });
    assert.equal(calls, 2);
    assert.throws(
      () =>
        captured.yieldPromise(() => {
          calls++;
          return Promise.resolve('stale');
        }),
      { kind: ZLinkFrameworkErrorKind.InvalidOperation }
    );
    assert.equal(calls, 2);
  } finally {
    serial.closeAdmission();
    await serial.whenIdle();
  }
});

test('HTTP factory throw preserves the current gate and consumes its Yield admission', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(true, 'http-owner');
  const scheduler = createIntegrationHttpExecutionScheduler({});
  const failure = new Error('request factory failed');
  const release = deferred();
  const attempted = deferred();
  const events = [];
  try {
    const owner = serial.execute(async () => {
      const turn = scheduler.capture();
      assert.throws(
        () =>
          turn.yieldPromise(() => {
            throw failure;
          }),
        (error) => error === failure
      );
      assert.throws(() => turn.yieldPromise(() => assert.fail('rejected factory ran')), {
        kind: ZLinkFrameworkErrorKind.InvalidOperation
      });
      events.push('owner');
      attempted.resolve();
      await release.promise;
      events.push('end');
    });
    const next = serial.execute(() => events.push('next'));
    await attempted.promise;
    assert.deepEqual(events, ['owner']);
    release.resolve();
    await Promise.all([owner, next]);
    assert.deepEqual(events, ['owner', 'end', 'next']);
  } finally {
    serial.closeAdmission();
    await serial.whenIdle();
  }
});

for (const ready of [false, true]) {
  test(`HTTP second Yield rejects before its request factory runs (ready=${ready})`, async () => {
    const serial = new ZLinkSpotSerialTurnExecutor(true, 'http-owner');
    const scheduler = createIntegrationHttpExecutionScheduler({});
    const client = ZLinkHttpClient.create('http://localhost').executionScheduler(scheduler).build();
    const response = { status: 200, headers: {}, body: { ok: true }, rawBody: '{"ok":true}' };
    const release = deferred();
    const attempted = deferred();
    let firstCalls = 0;
    let secondCalls = 0;
    let second;
    const owner = serial.execute(async () => {
      const firstRequest = client.get('/first');
      firstRequest.executeTyped = () => {
        firstCalls++;
        return ready ? Promise.resolve(response) : release.promise;
      };
      const secondRequest = client.get('/second');
      secondRequest.executeTyped = () => {
        secondCalls++;
        return Promise.resolve(response);
      };
      const first = firstRequest.yield();
      try {
        second = Promise.resolve(secondRequest.yield()).then(
          () => undefined,
          (error) => error
        );
      } catch (error) {
        second = Promise.resolve(error);
      }
      attempted.resolve();
      assert.equal(await first, response);
    });
    try {
      await attempted.promise;
      release.resolve(response);
      await owner;
      assert.equal((await second)?.kind, ZLinkFrameworkErrorKind.InvalidOperation);
      assert.equal(firstCalls, 1, 'admitted request factory must run once');
      assert.equal(secondCalls, 0, 'rejected request factory must never run');
      assert.equal(await serial.execute(() => 'PING'), 'PING');
    } finally {
      serial.closeAdmission();
      await serial.whenIdle();
      await client.close();
    }
  });
}
