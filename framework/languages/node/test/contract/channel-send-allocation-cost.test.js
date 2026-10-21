const assert = require('node:assert/strict');
const { createHook } = require('node:async_hooks');
const path = require('node:path');
const test = require('node:test');
const { channelSendFixture } = require('./fixtures/channel-send');
const {
  SubmitResult,
  ZLinkBackendResultError
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
const dist = path.resolve(__dirname, '../../packages/framework/dist');

function promiseCount(call) {
  let count = 0;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') count++;
    }
  });
  hook.enable();
  let result;
  try {
    result = call();
  } finally {
    hook.disable();
  }
  return { count, result };
}

for (const backend of [false, true, 'spot']) {
  const name = backend === 'spot' ? 'spot' : backend ? 'backend' : 'runtime';
  test(`${name} immediate channel sends create only the required boundary Promise`, async (t) => {
    const f = await channelSendFixture(dist, backend);
    try {
      for (const bytes of [1024, 4096]) {
        const payload = Buffer.alloc(bytes);
        const baseline = promiseCount(() => f.mockSubmit('peer', [payload])).count;
        const { count, result } = promiseCount(() => f.target.sendToChannel('channel', payload));
        assert.equal(await result, SubmitResult.Ok);
        t.diagnostic(JSON.stringify({ bytes, baseline, count, extra: count - baseline }));
        assert.equal(count - baseline, backend ? 1 : 0);
        const missing = promiseCount(() => f.target.sendToChannel('missing', payload));
        assert.equal(await missing.result, SubmitResult.NotFound);
        assert.equal(missing.count, backend ? 1 : 0);
        f.setSubmit(() => ({ result: SubmitResult.Backpressured, admitted: Promise.resolve() }));
        const deferredBaseline = promiseCount(() => f.mockSubmit('peer', [payload]));
        await deferredBaseline.result.admitted;
        const deferred = promiseCount(() => f.target.sendToChannel('channel', payload));
        assert.equal(await deferred.result, SubmitResult.Ok);
        assert.equal(deferred.count - deferredBaseline.count, 2);
        f.setSubmit(() => ({ result: SubmitResult.Ok }));
      }
    } finally {
      await f.close();
    }
  });

  test(`${name} channel send preserves absence, submission errors and deferred admission`, async () => {
    const f = await channelSendFixture(dist, backend);
    try {
      assert.equal(await f.target.sendToChannel('missing', Buffer.alloc(1)), SubmitResult.NotFound);
      for (const code of [SubmitResult.NotFound, SubmitResult.NotConnected]) {
        f.setSubmit(() => {
          throw new ZLinkBackendResultError('submit', code);
        });
        assert.equal(
          await f.target.sendToChannel('channel', Buffer.alloc(1)),
          SubmitResult.NotConnected
        );
      }
      for (const error of [
        new Error('unknown submission failure'),
        new ZLinkBackendResultError('submit', SubmitResult.Terminated)
      ]) {
        f.setSubmit(() => {
          throw error;
        });
        if (backend) {
          let pending;
          assert.doesNotThrow(() => {
            pending = f.target.sendToChannel('channel', Buffer.alloc(1));
          });
          assert.ok(pending instanceof Promise);
          await assert.rejects(pending, (value) => value === error);
        } else {
          assert.throws(
            () => f.target.sendToChannel('channel', Buffer.alloc(1)),
            (value) => value === error
          );
        }
      }
      for (const failure of [
        undefined,
        new ZLinkBackendResultError('submit', SubmitResult.NotFound, undefined, {
          phase: 'completion'
        }),
        new Error('deferred failure')
      ]) {
        let resolve, reject;
        const admitted = new Promise((yes, no) => {
          resolve = yes;
          reject = no;
        });
        f.setSubmit(() => ({ result: SubmitResult.Backpressured, admitted }));
        const pending = f.target.sendToChannel('channel', Buffer.alloc(1));
        assert.ok(pending instanceof Promise);
        let terminal = false;
        const observed = pending.then(
          (value) => {
            terminal = true;
            return value;
          },
          (error) => {
            terminal = true;
            throw error;
          }
        );
        await Promise.resolve();
        assert.equal(terminal, false);
        if (failure === undefined) resolve();
        else reject(failure);
        if (failure instanceof ZLinkBackendResultError)
          assert.equal(await observed, SubmitResult.NotConnected);
        else if (failure) await assert.rejects(observed, (value) => value === failure);
        else assert.equal(await observed, SubmitResult.Ok);
      }
    } finally {
      await f.close();
    }
  });
}

test('closed ServiceSpot channel send rejects through the Promise boundary', async () => {
  const f = await channelSendFixture(dist, 'spot');
  try {
    f.target.close();
    let pending;
    assert.doesNotThrow(() => {
      pending = f.target.sendToChannel('channel', Buffer.alloc(1024));
    });
    assert.ok(pending instanceof Promise);
    await assert.rejects(pending, /Spot '.*' is closed\./);
  } finally {
    await f.close();
  }
});
