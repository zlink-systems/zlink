const assert = require('node:assert/strict');
const { setTimeout: delay } = require('node:timers/promises');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const { wrapSocket } = require('../../packages/framework/dist/runtime/backend/node/node-socket-backend-adapter');
const { submitBindingPublish, submitBindingRequest } = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');

const OLD_SEND_TIMEOUT_MULTIPLE_MS = 3100;

function pendingStream() {
  let resolve;
  let reject;
  let submissions = 0;
  const admitted = new Promise((yes, no) => { resolve = yes; reject = no; });
  const operation = {
    message() { return this; },
    submit() {
      submissions += 1;
      return { result: zlink.SubmitResult.Backpressured, admitted };
    }
  };
  const socket = wrapSocket({
    close() {},
    recvPacket() {},
    send() { return operation; }
  });
  const stream = new framework.ZLinkManagedStream(socket, 'session');
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: {
      createTextMessage: text => zlink.Message.from(text),
      createBinaryMessage: bytes => zlink.Message.from(bytes)
    }
  });
  return {
    context: runtime.createSessionContext(stream), resolve, reject,
    get submissions() { return submissions; }
  };
}

test('public STREAM send stays pending beyond three old timeouts and completes once on admission', async () => {
  const fixture = pendingStream();
  let terminals = 0;
  const call = fixture.context.client.send({ id: 1 }).packetName('Notice');
  const pending = call.submit().finally(() => { terminals += 1; });
  await delay(OLD_SEND_TIMEOUT_MULTIPLE_MS);
  assert.equal(terminals, 0);
  assert.equal(fixture.submissions, 1);
  fixture.resolve();
  assert.equal(await pending, undefined);
  assert.equal(terminals, 1);
  assert.equal(fixture.submissions, 1);
  await assert.rejects(call.submit(), error => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation);
});

for (const [result, kind] of [
  [zlink.SubmitResult.NotFound, 'Unavailable'],
  [zlink.SubmitResult.Terminated, 'ShuttingDown']
]) {
  test(`pending public STREAM send ends with ${kind}`, async () => {
    const fixture = pendingStream();
    const pending = fixture.context.client.send({ id: result }).packetName('Notice').submit();
    const terminal = assert.rejects(pending, error => error.kind === framework.ZLinkFrameworkErrorKind[kind]);
    await delay(0);
    fixture.reject(new zlink.SubmitError(result, 0));
    await terminal;
    assert.equal(fixture.submissions, 1);
  });
}

test('Classic fanout publisher retains DeadlineExceeded', () => {
  const operation = {
    message() { return this; },
    submit() { throw new zlink.SubmitError(zlink.SubmitResult.Backpressured, 0); }
  };
  assert.throws(() => submitBindingPublish(operation, Buffer.from('event')),
    error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
});

test('request budget includes binding outbound admission', async () => {
  const never = new Promise(() => {});
  const operation = {
    message() { return this; },
    timeout() { return this; },
    submit() { return { result: zlink.SubmitResult.Backpressured, admitted: never, reply: never }; }
  };
  let guard;
  try {
    const missedBudget = new Promise((_, reject) => {
      guard = setTimeout(() => reject(new Error('request admission exceeded its budget')), 200);
    });
    await assert.rejects(Promise.race([submitBindingRequest(operation, Buffer.from('request'), 20), missedBudget]),
      error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
  } finally { clearTimeout(guard); }
});

test('backpressured request timeout observes late reply and closes each native message once', async () => {
  let completeReply;
  let closes = 0;
  const reply = new Promise(resolve => { completeReply = resolve; });
  const operation = {
    message() { return this; },
    timeout() { return this; },
    submit() { return { result: zlink.SubmitResult.Backpressured, admitted: Promise.resolve(), reply }; }
  };
  await assert.rejects(submitBindingRequest(operation, Buffer.from('request'), 20),
    error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
  completeReply([{ close() { closes += 1; } }]);
  await delay(0);
  assert.equal(closes, 1);
});
