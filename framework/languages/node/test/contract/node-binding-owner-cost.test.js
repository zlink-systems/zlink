const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const zlink = require('@zlink-systems/zlink');
const {
  submitBindingSend,
  submitBindingAsyncSend
} = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
const {
  ZLinkNodeRawBindingPort
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
function operation(snapshot) {
  return {
    message() {
      return this;
    },
    submit() {
      return snapshot;
    }
  };
}
test('immediate send retains the binding submit snapshot', () => {
  const snapshot = { result: zlink.SubmitResult.Ok, admitted: Promise.resolve() };
  assert.equal(submitBindingSend(operation(snapshot), Buffer.from('body')), snapshot);
});
test('immediate send adds no operation Promise and never observes the redundant admission stage', async () => {
  const snapshot = { result: zlink.SubmitResult.Ok, admitted: new Promise(() => {}) };
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') promises++;
    }
  });
  hook.enable();
  const result = submitBindingAsyncSend(operation(snapshot), Buffer.from('body'));
  hook.disable();
  assert.equal(promises, 0);
  await result;
});
test('send submit failures remain Promise rejections', async () => {
  const failure = new Error('submit failed');
  const source = {
    message() {
      return this;
    },
    submit() {
      throw failure;
    }
  };
  let result;
  assert.doesNotThrow(() => {
    result = submitBindingAsyncSend(source, Buffer.from('body'));
  });
  await assert.rejects(result, (error) => error === failure);
});
for (const kind of ['Router', 'Dealer']) {
  test(`${kind} request retains Buffer views while closing each native reply once`, async (t) => {
    const context = zlink.createContext();
    const factoryName = `create${kind}Socket`;
    const factory = zlink[factoryName];
    const replies = [
      zlink.Message.from(Buffer.from('header')),
      zlink.Message.from(Buffer.alloc(1024, 7))
    ];
    const views = replies.map((message) => message.data());
    let closes = 0;
    for (const message of replies) {
      const close = message.close.bind(message);
      t.mock.method(message, 'close', () => {
        closes++;
        close();
      });
      t.mock.method(message, 'toBytes', () => {
        throw new Error('Framework must retain the native Buffer view');
      });
    }
    t.mock.method(zlink, factoryName, (ctx) => {
      const socket = factory(ctx);
      t.mock.method(socket, 'request', () => {
        const builder = {
          message() {
            return this;
          },
          timeout() {
            return this;
          },
          submit() {
            return {
              result: zlink.SubmitResult.Ok,
              admitted: Promise.resolve(),
              reply: Promise.resolve(replies)
            };
          }
        };
        return builder;
      });
      return socket;
    });
    const host = new ZLinkNodeRawBindingPort(context).createHost();
    t.after(() => {
      host.close();
      context.close();
    });
    const socket = host[`create${kind}`]();
    const result =
      kind === 'Router'
        ? await socket.request('target', [Buffer.from('request')], 1000)
        : await socket.request([Buffer.from('request')], 1000);
    assert.equal(closes, replies.length);
    assert.equal(result[0], views[0]);
    assert.equal(result[1], views[1]);
    assert.equal(result[0].toString(), 'header');
    assert.equal(result[1].length, 1024);
    assert.equal(result[1][0], 7);
  });
}
