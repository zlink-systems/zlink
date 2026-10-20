const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate } = require('node:timers/promises');
const framework = require('../../packages/framework/dist/internal');
const { mapZLinkRequestReply, identityRequestReply } = require('../../packages/framework/dist/runtime/messaging/request-result');

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

function fixture(shared) {
  const admissions = [];
  const replies = [];
  const registration = framework.createFrameworkRegistration({
    spotNodes: { mesh: { router: { bind: 'tcp://127.0.0.1:0' } } }
  });
  const client = new framework.DefaultZLinkRouteClient(registration, {
    request(_mesh, _rid, _packet, _request, _timeout, _signal, _metadata, onAdmitted, observer) {
      if (shared !== undefined) return mapZLinkRequestReply(shared, identityRequestReply, observer);
      const admission = deferred();
      const reply = deferred();
      admissions.push(admission);
      replies.push(reply);
      void admission.promise.then(onAdmitted, () => undefined);
      return mapZLinkRequestReply(reply.promise, identityRequestReply, observer);
    }
  });
  return {
    admissions, replies,
    submit: () => client.requestToNode('mesh', 'peer', {}).submit()
  };
}

test('public request exposes admission before reply and preserves await', async () => {
  const f = fixture();
  const pending = f.submit();
  assert.ok(pending instanceof Promise);
  assert.ok(pending.admitted instanceof Promise);
  let replied = false;
  void pending.then(() => { replied = true; });
  f.admissions[0].resolve();
  await pending.admitted;
  assert.equal(replied, false);
  f.replies[0].resolve('reply');
  assert.equal(await pending, 'reply');
});

test('capacity wait keeps both public stages pending', async () => {
  const f = fixture();
  const pending = f.submit();
  assert.ok(pending.admitted instanceof Promise);
  let admitted = false;
  let replied = false;
  void pending.admitted.then(() => { admitted = true; });
  void pending.then(() => { replied = true; });
  await setImmediate();
  assert.equal(admitted, false);
  assert.equal(replied, false);
  f.admissions[0].resolve();
  await pending.admitted;
  assert.equal(replied, false);
  f.replies[0].resolve('reply');
  await pending;
});

for (const reason of ['timeout', 'cancellation']) {
  test(`failure before admission has the same cause in both stages (${reason})`, async () => {
    const f = fixture();
    const pending = f.submit();
    assert.ok(pending.admitted instanceof Promise);
    const error = new Error(reason);
    const stages = Promise.allSettled([pending.admitted, pending]);
    f.admissions[0].reject(error);
    f.replies[0].reject(error);
    const results = await stages;
    assert.equal(results[0].reason, error);
    assert.equal(results[1].reason, error);
  });
}

test('request admission expressions are isolated across submissions', async () => {
  const f = fixture();
  const first = f.submit();
  const second = f.submit();
  assert.ok(first.admitted instanceof Promise);
  assert.notEqual(first.admitted, second.admitted);
  let secondAdmitted = false;
  void second.admitted.then(() => { secondAdmitted = true; });
  f.admissions[0].resolve();
  await first.admitted;
  await setImmediate();
  assert.equal(secondAdmitted, false);
  f.replies[0].resolve(1);
  f.admissions[1].resolve();
  f.replies[1].resolve(2);
  assert.deepEqual(await Promise.all([first, second]), [1, 2]);
});

test('shared completed transport results yield independent public reply and admission expressions', async () => {
  const shared = Object.defineProperty(Promise.resolve(7), 'admitted', {
    value: Promise.resolve()
  });
  const f = fixture(shared);
  const first = f.submit();
  const second = f.submit();
  assert.notEqual(first, second);
  assert.notEqual(first.admitted, second.admitted);
  await first.admitted;
  assert.deepEqual(await Promise.all([first, second]), [7, 7]);
  const third = f.submit();
  assert.notEqual(first.admitted, third.admitted);
  await third.admitted;
  assert.equal(await third, 7);
});

for (const wrapped of [false, true]) {
  test(`successful reply completes admission without an observer (delivery wrapper=${wrapped})`, async () => {
    const {
      createZLinkRequest
    } = require('../../packages/framework/dist/runtime/messaging/request-result');
    const reply = deferred();
    const pending = createZLinkRequest(
      (_admitted, observer) => mapZLinkRequestReply(reply.promise, identityRequestReply, observer),
      wrapped ? (value) => value.then((result) => result) : undefined
    );
    let admitted = false;
    void pending.admitted.then(() => {
      admitted = true;
    });
    await setImmediate();
    assert.equal(admitted, false);
    reply.resolve('reply');
    assert.equal(await pending, 'reply');
    await setImmediate();
    assert.equal(admitted, true);
  });
}
