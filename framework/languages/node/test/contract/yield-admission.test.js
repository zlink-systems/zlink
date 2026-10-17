const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  DefaultZLinkSpotOutbound
} = require('../../packages/framework/dist/runtime/spots/spot-outbound');
const {
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/contracts/Errors/ZLinkFrameworkException');
const tick = () => new Promise((resolve) => setImmediate(resolve));
function deferred() {
  let resolve;
  const promise = new Promise((r) => {
    resolve = r;
  });
  return { promise, resolve };
}
function fixture(ready = false) {
  const serial = new ZLinkSpotSerialTurnExecutor(true, 'hub');
  const calls = [];
  const outbound = new DefaultZLinkSpotOutbound({
    serial,
    addressTransport: {
      requestToSpotAddress(id) {
        const result = deferred();
        calls.push({ id, ...result });
        return ready ? Promise.resolve(id) : result.promise;
      }
    }
  });
  return {
    serial,
    calls,
    leaf: (id) => outbound.requestToSpot(id, {}).instanceSpot('leaf').yield()
  };
}
for (const ready of [false, true]) {
  test(`Yield rejects an unawaited second call before submission (ready=${ready})`, async () => {
    const f = fixture(ready);
    let error;
    let first;
    const owner = f.serial.execute(async () => {
      first = f.leaf('first');
      let second;
      try {
        second = f.leaf('second');
      } catch (e) {
        error = e;
        second = Promise.resolve();
      }
      const both = Promise.all([first, second]);
      if (error) await both;
    });
    await tick();
    assert.equal(f.calls.length, 1, 'second call must not reach transport');
    assert.equal(error?.kind, ZLinkFrameworkErrorKind.InvalidOperation);
    f.calls[0].resolve('first');
    await owner;
    await first;
    assert.equal(await f.serial.execute(() => 'PING'), 'PING');
    f.serial.closeAdmission();
    await f.serial.whenIdle();
  });
}
test('Yield can be used again after sequential resume', async () => {
  const f = fixture();
  const owner = f.serial.execute(async () => [await f.leaf('a'), await f.leaf('b')]);
  await tick();
  assert.equal(f.calls.length, 1);
  f.calls[0].resolve('a');
  await tick();
  assert.equal(f.calls.length, 2);
  f.calls[1].resolve('b');
  assert.deepEqual(await owner, ['a', 'b']);
  assert.equal(await f.serial.execute(() => 'PING'), 'PING');
  f.serial.closeAdmission();
  await f.serial.whenIdle();
});
test('Yield rejects a completed handler async tail before submission', async () => {
  const f = fixture();
  const release = deferred();
  const observed = deferred();
  await f.serial.execute(() => {
    void release.promise.then(() => {
      try {
        f.leaf('tail');
        observed.resolve(undefined);
      } catch (error) {
        observed.resolve(error);
      }
    });
  });
  release.resolve();
  const error = await observed.promise;
  assert.equal(f.calls.length, 0, 'owner turn has ended: no transport submission');
  assert.equal(error?.kind, ZLinkFrameworkErrorKind.InvalidOperation);
  f.serial.closeAdmission();
  await f.serial.whenIdle();
});
test('failed Yield reacquires the gate before its catch and next Yield', async () => {
  const f = fixture();
  let rejectResult;
  const failed = new Promise((_, reject) => {
    rejectResult = reject;
  });
  const blockerStarted = deferred();
  const releaseBlocker = deferred();
  const failure = new Error('transport failed');
  let caught = false;
  const owner = f.serial.execute(async () => {
    try {
      await f.serial.yieldPromise(failed);
    } catch (error) {
      assert.equal(error, failure);
      caught = true;
    }
    return await f.leaf('next');
  });
  const blocker = f.serial.execute(async () => {
    blockerStarted.resolve();
    await releaseBlocker.promise;
  });
  await blockerStarted.promise;
  rejectResult(failure);
  await tick();
  assert.equal(caught, false, 'failure continuation must wait for the gate');
  releaseBlocker.resolve();
  await blocker;
  await tick();
  assert.equal(caught, true);
  assert.equal(f.calls.length, 1);
  f.calls[0].resolve('next');
  assert.equal(await owner, 'next');
  f.serial.closeAdmission();
  await f.serial.whenIdle();
});
test('Yield rejects a second call from the suspended async context', async () => {
  const f = fixture();
  const releaseSecond = deferred();
  const attempted = deferred();
  let error;
  const owner = f.serial.execute(async () => {
    const first = f.leaf('first');
    await releaseSecond.promise;
    try {
      f.leaf('second');
    } catch (e) {
      error = e;
    }
    attempted.resolve();
    if (error) await first;
  });
  await tick();
  releaseSecond.resolve();
  await attempted.promise;
  assert.equal(f.calls.length, 1);
  assert.equal(error?.kind, ZLinkFrameworkErrorKind.InvalidOperation);
  f.calls[0].resolve('first');
  await owner;
  assert.equal(await f.serial.execute(() => 'PING'), 'PING');
  f.serial.closeAdmission();
  await f.serial.whenIdle();
});
