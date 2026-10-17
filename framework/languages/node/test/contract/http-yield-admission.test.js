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

test('HTTP factory throw finishes through Yield before its catch and the next Yield', async () => {
  const serial = new ZLinkSpotSerialTurnExecutor(true, 'http-owner');
  const scheduler = createIntegrationHttpExecutionScheduler({});
  const failure = new Error('request factory failed');
  const release = deferred();
  const blockerStarted = deferred();
  const events = [];
  let synchronousFailure;
  let caught = false;
  let calls = 0;
  try {
    const owner = serial.execute(async () => {
      const turn = scheduler.capture();
      let pending;
      try {
        pending = turn.yieldPromise(() => {
          calls++;
          throw failure;
        });
      } catch (error) {
        synchronousFailure = error;
        pending = Promise.reject(error);
      }
      assert.throws(() => turn.yieldPromise(() => assert.fail('rejected factory ran')), {
        kind: ZLinkFrameworkErrorKind.InvalidOperation
      });
      try {
        await pending;
        assert.fail('factory failure must reject the Yield');
      } catch (error) {
        assert.equal(error, failure);
        caught = true;
        events.push('catch');
      }
      assert.equal(await turn.yieldPromise(() => Promise.resolve('resumed')), 'resumed');
      events.push('end');
    });
    const blocker = serial.execute(async () => {
      events.push('blocker');
      blockerStarted.resolve();
      await release.promise;
      events.push('blocker:end');
    });
    const completed = Promise.allSettled([owner, blocker]);
    try {
      await blockerStarted.promise;
      assert.equal(synchronousFailure, undefined, 'factory throw must become a rejected Promise');
      assert.equal(caught, false, 'failure catch must wait for the gate');
      assert.deepEqual(events, ['blocker']);
    } finally {
      release.resolve();
      await completed;
    }
    assert.equal((await completed)[0].status, 'fulfilled');
    assert.equal(calls, 1);
    assert.deepEqual(events, ['blocker', 'blocker:end', 'catch', 'end']);
    assert.equal(
      await serial.execute(() => scheduler.capture().yieldPromise(() => Promise.resolve('next'))),
      'next'
    );
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
