const assert = require('node:assert/strict');
const test = require('node:test');
const api = require('../dist');
const { ZlinkStreamReceivedMessages } = require('../dist/Runtime/ZlinkStreamReceivedMessages');
const { ZlinkStreamConnectorEvents } = require('../dist/Runtime/ZlinkStreamConnectorEvents');
const {
  DefaultZlinkStreamActor,
  ZlinkStreamActors,
  zlinkStreamActorBinding
} = require('../dist/Runtime/ZlinkStreamActors');
const { ZlinkStreamHeaderCodec } = require('../dist/Runtime/Protocol/ZlinkStreamHeaderCodec');
const wire = require('@zlink-systems/stream-wire');
const { namedReplyHeader } = require('../../../test/contract/helpers/named-reply-header');
const { ZlinkStreamReceiveDispatcher } = require('../dist/Runtime/ZlinkStreamReceiveDispatcher');
const { ZlinkStreamMetadataCodec } = require('../dist/Runtime/Protocol/ZlinkStreamMetadataCodec');

test('N02 registration identity preserves the callback invocation receiver', async () => {
  const { queue, events } = runtime();
  const receivers = [];
  const observe = function () {
    'use strict';
    receivers.push(this);
  };
  events.onDisconnected(observe);
  events.publishDisconnected();
  queue.pump();
  const actors = new ZlinkStreamActors({}, queue, events);
  actors.onBound(observe);
  actors.processControl('$zlink.actor.bound', new Uint8Array([1, 0, 1, 1, 65]));
  queue.pump();
  const instance = api.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://connector-node.invalid',
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  });
  instance.onRequestSending(observe);
  instance.onReplyReceived(observe);
  await assert.rejects(
    instance
      .request({ codec: api.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
      .packetName('Request')
      .submit()
  );
  await instance.dispatch();
  await instance.close();
  assert.deepEqual(receivers, [undefined, undefined, undefined, undefined]);
});

function runtime() {
  let queue;
  const events = new ZlinkStreamConnectorEvents((run, count) => queue.enqueueCallback(run, count));
  queue = new ZlinkStreamReceivedMessages(events, false);
  return { queue, events };
}
function message(name, actor) {
  return {
    name,
    metadata: api.ZlinkStreamMetadataMap.empty,
    payload: { codec: api.ZlinkStreamCodec.Raw, payload: new Uint8Array() },
    [zlinkStreamActorBinding]: actor
  };
}

test('N01 closed Actor handlers are excluded before and during dispatch', () => {
  for (const closeBefore of [false, true]) {
    const { queue } = runtime();
    const actor = new DefaultZlinkStreamActor({}, 'A', 1);
    const calls = [];
    queue.on(
      'Push',
      () => {
        calls.push('first');
        actor.close();
      },
      actor
    );
    queue.on('Push', () => calls.push('second'), actor);
    queue.enqueue(message('Push', actor));
    if (closeBefore) actor.close();
    queue.pump();
    assert.deepEqual(calls, closeBefore ? [] : ['first']);
  }
});

test('N02 push disposal excludes the original registration and defers additions', () => {
  const { queue } = runtime();
  const calls = [];
  const handler = () => calls.push('second');
  let second;
  queue.on('Push', () => {
    calls.push('first');
    second.dispose();
    queue.on('Push', handler);
  });
  second = queue.on('Push', handler);
  queue.enqueue(message('Push'));
  queue.pump();
  assert.deepEqual(calls, ['first']);
});

test('N02 event duplicate registrations have independent lifetimes', () => {
  const { queue, events } = runtime();
  let calls = 0;
  const fn = () => calls++;
  const first = events.onDisconnected(fn);
  const second = events.onDisconnected(fn);
  second.dispose();
  second.dispose();
  events.publishDisconnected();
  queue.pump();
  assert.equal(calls, 1);
  first.dispose();
  events.publishDisconnected();
  queue.pump();
  assert.equal(calls, 1);
});

test('N02 event disposal inside a callback excludes its successor', () => {
  const { queue, events } = runtime();
  const calls = [];
  let second;
  events.onDisconnected(() => {
    calls.push('first');
    second.dispose();
  });
  second = events.onDisconnected(() => calls.push('second'));
  events.publishDisconnected();
  queue.pump();
  assert.deepEqual(calls, ['first']);
});

test('N02 Actor lifecycle duplicates have independent lifetimes', () => {
  const { queue, events } = runtime();
  const actors = new ZlinkStreamActors({}, queue, events);
  let calls = 0;
  const fn = () => calls++;
  actors.onBound(fn);
  actors.onBound(fn).dispose();
  actors.processControl('$zlink.actor.bound', new Uint8Array([1, 0, 1, 1, 65]));
  queue.pump();
  assert.equal(calls, 1);
});

test('N04 reserved inbound names have protocol decode errors', () => {
  const header = wire.encodeStreamWireHeader({
    kind: api.ZlinkStreamMessageKind.Send,
    codec: api.ZlinkStreamCodec.Raw,
    flags: 0,
    name: '$zlink.bad',
    metadata: new Map()
  });
  assert.throws(
    () => ZlinkStreamHeaderCodec.decode(header),
    (e) => e.error?.code === api.ZlinkStreamErrorCode.FrameDecodeFailed
  );
});

test('N03 malformed Error payload keeps the connection for sequenced and unsequenced frames', async () => {
  for (const requestSeq of [undefined, 1n]) {
    const { queue, events } = runtime();
    const errors = [];
    events.onError((error) => errors.push(error.code));
    const header = {
      kind: api.ZlinkStreamMessageKind.Error,
      codec: api.ZlinkStreamCodec.Json,
      name: '',
      flags: 0,
      requestSeq,
      metadata: api.ZlinkStreamMetadataMap.empty
    };
    const dispatcher = new ZlinkStreamReceiveDispatcher(
      {
        decodeFrames: () => [{ header, payload: new TextEncoder().encode('{}') }],
        decodePayload: (_, bytes) => bytes
      },
      { reject: () => false },
      queue,
      {},
      events,
      {}
    );
    await dispatcher.readAndDispatch({ read: async () => new Uint8Array() });
    queue.pump();
    assert.deepEqual(errors, [api.ZlinkStreamErrorCode.FrameDecodeFailed]);
  }
});

test('N02 removed error handler is excluded from queued callback failure delivery', () => {
  const { queue, events } = runtime();
  const pending = [];
  const recordingEvents = new ZlinkStreamConnectorEvents((run) => pending.push(run));
  let calls = 0;
  recordingEvents.onError(() => {
    throw new Error('callback failure');
  });
  const second = recordingEvents.onError(() => calls++);
  recordingEvents.publishError({ code: api.ZlinkStreamErrorCode.RemoteError, message: 'remote' });
  pending.shift()();
  assert.equal(calls, 1);
  second.dispose();
  while (pending.length > 0) pending.shift()();
  assert.equal(calls, 1);
  // The regular queue also accepts callbacks whose live registration count is zero.
  events.publishDisconnected();
  queue.pump();
});

test('N05 metadata accepts UTF-8 and equals, empty keys use the dedicated error', () => {
  for (const name of ['tenant', '지역', 'a=b', '😀', '\ud800']) {
    const metadata = api.ZlinkStreamMetadataMap.empty.with(name, '값');
    assert.equal(metadata.get(name), '값');
  }
  assert.throws(
    () => api.ZlinkStreamMetadataMap.empty.with('', 'x'),
    (e) =>
      e instanceof api.ZlinkStreamException &&
      e.error.code === api.ZlinkStreamErrorCode.ValidationFailed
  );
});

test('N05 valid UTF-8 metadata round-trips through the wire header', () => {
  const metadata = api.ZlinkStreamMetadataMap.empty.with('지역', '서울').with('a=b', '😀');
  const header = {
    kind: api.ZlinkStreamMessageKind.Send,
    codec: api.ZlinkStreamCodec.Raw,
    name: 'Push',
    flags: 0,
    metadata
  };
  const received = ZlinkStreamHeaderCodec.decode(ZlinkStreamHeaderCodec.encode(header));
  assert.equal(received.metadata.get('지역'), '서울');
  assert.equal(received.metadata.get('a=b'), '😀');
});

test('N06 TLS validation belongs to the closed error set', () => {
  assert.equal(api.ZlinkStreamErrorCode.TlsValidationFailed, 'tlsValidationFailed');
  assert.equal(Object.values(api.ZlinkStreamErrorCode).length, 13);
});

test('N07 repeated dispatch preserves the undelivered head and dispatch order', () => {
  const { queue } = runtime();
  let calls = 0;
  queue.enqueue(message('Later'));
  queue.on('Push', () => calls++);
  for (let i = 0; i < 10000; i++) {
    queue.enqueue(message('Push'));
    queue.pump();
  }
  assert.equal(calls, 10000);
  queue.on('Later', () => calls++);
  queue.pump();
  assert.equal(calls, 10001);
});

test('N08 only open Actors close in issue order and closed handles remain readable', () => {
  const { queue, events } = runtime();
  const actors = new ZlinkStreamActors({}, queue, events);
  const closed = [];
  actors.onUnbound((actor) => closed.push(actor.actorId));
  actors.processControl('$zlink.actor.bound', new Uint8Array([1, 0, 1, 1, 65]));
  const first = actors.find('A');
  actors.processControl('$zlink.actor.bound', new Uint8Array([1, 0, 2, 1, 66]));
  actors.processControl('$zlink.actor.unbound', new Uint8Array([1, 0, 1]));
  queue.pump();
  actors.processControl('$zlink.actor.bound', new Uint8Array([1, 0, 1, 1, 67]));
  actors.closeAll();
  queue.pump();
  assert.deepEqual(closed, ['A', 'B', 'C']);
  assert.deepEqual(actors.snapshot, []);
  assert.equal(first.actorId, 'A');
  assert.equal(first.isBound, false);
});

test('N09 metadata size matches TextEncoder for surrogate boundaries and enforces the limit', () => {
  const encoder = new TextEncoder();
  for (const text of [
    '',
    'ascii',
    '지역',
    '😀',
    '\ud800',
    '\udfff',
    '\ud800x\udfff',
    '\ud800\ud800\udc00'
  ]) {
    const metadata = api.ZlinkStreamMetadataMap.empty.with('key', text);
    assert.equal(ZlinkStreamMetadataCodec.size(metadata), 1 + 3 + 3 + encoder.encode(text).length);
  }
  assert.equal(ZlinkStreamMetadataCodec.size(api.ZlinkStreamMetadataMap.empty), 0);
  assert.equal(
    ZlinkStreamMetadataCodec.size(api.ZlinkStreamMetadataMap.empty.with('k', 'x'.repeat(1019))),
    1024
  );
  assert.throws(
    () =>
      ZlinkStreamMetadataCodec.size(api.ZlinkStreamMetadataMap.empty.with('k', 'x'.repeat(1020))),
    (e) => e.error?.code === api.ZlinkStreamErrorCode.ValidationFailed
  );
});

test('N09 enum validation rejects unknown numeric values and reverse mapping names', () => {
  const header = {
    kind: api.ZlinkStreamMessageKind.Send,
    codec: api.ZlinkStreamCodec.Raw,
    name: 'Push',
    flags: 0,
    metadata: api.ZlinkStreamMetadataMap.empty
  };
  for (const invalid of [{ kind: 99 }, { kind: 'Send' }, { codec: 99 }, { codec: 'Raw' }]) {
    assert.throws(
      () => ZlinkStreamHeaderCodec.encode({ ...header, ...invalid }),
      (e) => e instanceof api.ZlinkStreamException
    );
  }
  assert.equal(ZlinkStreamHeaderCodec.decode(ZlinkStreamHeaderCodec.encode(header)).name, 'Push');
});

test('N02 request and reply hooks preserve registration identity during dispatch', async () => {
  const instance = api.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://connector-node.invalid',
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  });
  const sending = [];
  const replied = [];
  const send = () => sending.push('second');
  const reply = () => replied.push('second');
  let secondSend;
  let secondReply;
  instance.onRequestSending(() => {
    sending.push('first');
    secondSend.dispose();
    instance.onRequestSending(send);
  });
  secondSend = instance.onRequestSending(send);
  instance.onReplyReceived(() => {
    replied.push('first');
    secondReply.dispose();
    instance.onReplyReceived(reply);
  });
  secondReply = instance.onReplyReceived(reply);
  await assert.rejects(
    instance
      .request({ codec: api.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
      .packetName('Request')
      .submit()
  );
  await instance.dispatch();
  assert.deepEqual(sending, ['first']);
  assert.deepEqual(replied, ['first']);
  await instance.close();
});

test('F20 connector preserves valid names and maps named replies to FrameDecodeFailed', () => {
  for (const kind of [api.ZlinkStreamMessageKind.Send, api.ZlinkStreamMessageKind.Request]) {
    const header = wire.encodeStreamWireHeader({
      kind,
      codec: api.ZlinkStreamCodec.Raw,
      flags: 0,
      requestSeq: kind === api.ZlinkStreamMessageKind.Request ? 1n : undefined,
      name: 'Push',
      metadata: new Map()
    });
    assert.equal(ZlinkStreamHeaderCodec.decode(header).name, 'Push');
  }
  for (const kind of [api.ZlinkStreamMessageKind.Response, api.ZlinkStreamMessageKind.Error]) {
    const header = wire.encodeStreamWireHeader({
      kind,
      codec: api.ZlinkStreamCodec.Json,
      flags: 0,
      requestSeq: 1n,
      name: '',
      metadata: new Map()
    });
    assert.equal(ZlinkStreamHeaderCodec.decode(header).name, '');
    const named = namedReplyHeader(kind);
    assert.throws(
      () => ZlinkStreamHeaderCodec.decode(named),
      (error) =>
        error instanceof api.ZlinkStreamException &&
        error.error.code === api.ZlinkStreamErrorCode.FrameDecodeFailed
    );
  }
});

test(
  'F20 a named reply ends the public connector with FrameDecodeFailed and ProtocolError',
  { timeout: 2000 },
  async () => {
    for (const kind of [api.ZlinkStreamMessageKind.Response, api.ZlinkStreamMessageKind.Error]) {
      let deliver;
      let readStarted;
      const reading = new Promise((resolve) => {
        readStarted = resolve;
      });
      let transportClosed = false;
      const instance = api.zlinkStreamConnectorFactory.create({
        endpoint: 'ws://connector-node.invalid',
        heartbeat: { enabled: false },
        reconnect: { enabled: false },
        dispatchMode: api.ZlinkStreamDispatchMode.Immediate,
        transportFactory: {
          connect: async () => ({
            write: async () => {},
            close: async () => {
              transportClosed = true;
            },
            read: () =>
              new Promise((resolve) => {
                deliver = resolve;
                readStarted();
              })
          })
        }
      });
      const errors = [];
      instance.onErrorReceived((error) => errors.push(error.code));
      const disconnected = new Promise((resolve) => instance.onDisconnected(resolve));
      try {
        await instance.connect();
        await reading;
        const named = namedReplyHeader(kind);
        deliver(wire.encodeStreamWireFrame(named, new Uint8Array()));
        await disconnected;
        assert.equal(instance.state, api.ZlinkStreamConnectionState.Disconnected);
        assert.equal(instance.closeReason, 'ProtocolError');
        assert.equal(transportClosed, true);
        assert.deepEqual(errors, [api.ZlinkStreamErrorCode.FrameDecodeFailed]);
      } finally {
        await instance.close();
      }
    }
  }
);
