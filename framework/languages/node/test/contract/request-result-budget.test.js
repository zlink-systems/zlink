const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const { createZLinkRequest } = require('../../packages/framework/dist/runtime/messaging/request-result');

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((done, fail) => { resolve = done; reject = fail; });
  return { promise, resolve, reject };
}

test('reply-only requests allocate only the public reply observation Promise', async () => {
  const pending = deferred();
  let markAdmitted;
  let count = 0;
  const hook = createHook({ init(_id, type) { if (type === 'PROMISE') count++; } });
  hook.enable();
  let request;
  try {
    request = createZLinkRequest((admitted) => { markAdmitted = admitted; return pending.promise; });
  } finally {
    hook.disable();
  }
  markAdmitted();
  pending.resolve('reply');
  assert.equal(await request, 'reply');
  await request.admitted;
  assert.equal(count, 1);
});

test('observed admission completes before reply and retains one public Promise', async () => {
  const pending = deferred();
  let markAdmitted;
  const request = createZLinkRequest((admitted) => { markAdmitted = admitted; return pending.promise; });
  const admitted = request.admitted;
  assert.equal(request.admitted, admitted);
  let replied = false;
  void request.then(() => { replied = true; });
  markAdmitted();
  markAdmitted();
  await admitted;
  assert.equal(replied, false);
  pending.resolve('reply');
  assert.equal(await request, 'reply');
  assert.equal(request.admitted, admitted);
});

test('admission retains its first outcome when a later reply fails', async () => {
  const pending = deferred();
  let markAdmitted;
  const request = createZLinkRequest((admitted) => { markAdmitted = admitted; return pending.promise; });
  const failure = new Error('reply failed after admission');
  const observed = assert.rejects(request, (error) => error === failure);
  markAdmitted();
  pending.reject(failure);
  await observed;
  await request.admitted;
});

test('a failure before admission rejects both lazy public stages with the same cause', async () => {
  const failure = new Error('submit failed');
  const request = createZLinkRequest(() => { throw failure; });
  const observed = assert.rejects(request, (error) => error === failure);
  await observed;
  await assert.rejects(request.admitted, (error) => error === failure);
  assert.equal(request.admitted, request.admitted);
});

test('one request admission failure does not alter a completed or waiting sibling stage', async () => {
  const left = deferred();
  const right = deferred();
  let admitLeft;
  const first = createZLinkRequest((admitted) => { admitLeft = admitted; return left.promise; });
  const second = createZLinkRequest(() => right.promise);
  const failure = new Error('sibling admission failed');
  const observed = assert.rejects(second, (error) => error === failure);
  const secondAdmission = second.admitted;
  const admissionFailure = assert.rejects(secondAdmission, (error) => error === failure);
  admitLeft();
  await first.admitted;
  right.reject(failure);
  await Promise.all([observed, admissionFailure]);
  await first.admitted;
  left.resolve('first reply');
  assert.equal(await first, 'first reply');
});
