const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: immediate } = require('node:timers/promises');
const zlink = require('@zlink-systems/zlink');
const {
  ZLinkNodeRawBindingPort
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');

// Spec 08 I3/I4 and binding Received: snapshots own their managed payload,
// while the socket reuses its receive envelope. Delayed replies retain tokens.
test('raw router reuses receive storage while earlier requests keep payload and reply tokens', async (t) => {
  const context = zlink.createContext();
  const source = zlink.createRouterSocket(context);
  const create = zlink.createRouterSocket;
  const storage = new Set();
  t.mock.method(zlink, 'createRouterSocket', (ctx) => {
    const socket = create(ctx);
    const recv = socket.recv;
    t.mock.method(socket, 'recv', function (received, flags) {
      storage.add(received);
      return recv.call(this, received, flags);
    });
    return socket;
  });
  const host = new ZLinkNodeRawBindingPort(context).createHost();
  const target = host.createRouter();
  const address = `inproc://raw-receive-storage-${process.pid}`;
  const targetId = zlink.RoutingId.from('target');
  target.setRoutingId('target');
  target.bind(address);
  source.setRoutingId(zlink.RoutingId.from('source'));
  source.options.setConnectRoutingId(targetId);
  source.connect(address);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  poller.add(source, [zlink.PollEventFlag.PollCompletion], 0);
  const records = [];
  try {
    while (source.routesSnapshot().length === 0) await immediate();
    const requests = [];
    const count = 16;
    for (let index = 0; index < count; index++) {
      requests.push(
        source
          .request(targetId)
          .message(Buffer.from(`request-${index}`))
          .submit()
      );
    }
    while (records.length < count) {
      const record = target.receive(true);
      if (record === undefined) await immediate();
      else records.push(record);
    }
    assert.equal(target.receive(true), undefined);
    for (let index = count - 1; index >= 0; index--) {
      assert.equal(records[index].parts[0].toString(), `request-${index}`);
      records[index].reply([Buffer.from(`reply-${index}`)]);
      records[index].close();
    }
    let complete = false;
    const replies = Promise.all(requests.map((request) => request.reply));
    replies.then(() => {
      complete = true;
    });
    while (!complete) {
      poller.wait(events, 0);
      await immediate();
    }
    (await replies).forEach((parts, index) => {
      assert.equal(parts[0].data().toString(), `reply-${index}`);
      for (const part of parts) part.close();
    });
    host.close();
    assert.equal(records[0].parts[0].toString(), 'request-0');
    t.diagnostic(`binding Received instances for ${count} requests: ${storage.size}`);
    assert.equal(storage.size, 1);
  } finally {
    for (const record of records) record.close();
    events.close();
    poller.close();
    host.close();
    source.close();
    context.close();
  }
});
