const assert = require('node:assert/strict');
const test = require('node:test');

const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');
const json = connector;

test('default connector rejects non-finite numbers on receive', () => {
  assert.throws(
    () =>
      json.fromJson({
        codec: connector.ZlinkStreamCodec.Json,
        payload: new TextEncoder().encode('{"ratio":1e400}')
      }),
    /finite/
  );
});

test('default connector shares framework-json-v1 integer encoding and syntax validation', () => {
  const value = {
    min: -(1n << 63n),
    max: (1n << 63n) - 1n,
    unsigned: (1n << 64n) - 1n,
    small: 2147483647
  };
  const encoded = json.toJson(value);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(encoded.payload)), {
    min: '-9223372036854775808',
    max: '9223372036854775807',
    unsigned: '18446744073709551615',
    small: 2147483647
  });
  for (const text of ['{"x":1,"x":2}', '\uFEFF{"x":1}', '{"x":1,}', '{/*comment*/"x":1}']) {
    assert.throws(() =>
      json.fromJson({
        codec: connector.ZlinkStreamCodec.Json,
        payload: new TextEncoder().encode(text)
      })
    );
  }
  assert.throws(() => json.toJson({ value: 1n << 64n }), /range/);
  assert.throws(() => json.toJson({ value: new Date(0) }), /Date/);
  assert.throws(() => json.toJson({ value: Infinity }), /finite/);
  assert.equal(
    json.fromJson(json.toJson({ decimalText: '9223372036854775807' })).decimalText,
    '9223372036854775807'
  );
});

test('default connector decodes server integer strings using the existing typed packet contract', () => {
  const framework = require('../../packages/framework/dist');
  class Counters {}
  framework.ZLinkPacket('profile.counters', {
    payload: {
      type: 'object',
      properties: {
        min: { type: 'int64' },
        max: { type: 'int64' },
        unsigned: { type: 'uint64' },
        small: { type: 'int32' },
        name: { type: 'string' }
      },
      required: ['min', 'max', 'unsigned', 'small', 'name']
    }
  })(Counters);
  const value = {
    min: -(1n << 63n),
    max: (1n << 63n) - 1n,
    unsigned: (1n << 64n) - 1n,
    small: 2147483647,
    name: 'player'
  };
  const encoded = json.toJson(value, Counters);
  assert.deepEqual(json.zlinkStreamJsonCodec.decode(encoded, Counters), value);
  for (const change of [
    { min: '0001' },
    { min: 1 },
    { name: null },
    { min: '9223372036854775808' }
  ]) {
    const invalid = { ...JSON.parse(new TextDecoder().decode(encoded.payload)), ...change };
    assert.throws(() =>
      json.zlinkStreamJsonCodec.decode(
        {
          codec: connector.ZlinkStreamCodec.Json,
          payload: new TextEncoder().encode(JSON.stringify(invalid))
        },
        Counters
      )
    );
  }
});

test('default connector uses enum names and rejects ordinal representations in typed DTOs', () => {
  const framework = require('../../packages/framework/dist');
  class ProfileReportReq {}
  framework.ZLinkPacket('profile.report', {
    payload: {
      type: 'object',
      properties: {
        reasonId: { type: 'enum', names: ['nickname', 'profile', 'harassment', 'other'] }
      },
      required: ['reasonId']
    }
  })(ProfileReportReq);

  const value = { reasonId: 'profile' };
  const encoded = json.toJson(value, ProfileReportReq);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(encoded.payload)), value);
  assert.deepEqual(json.zlinkStreamJsonCodec.decode(encoded, ProfileReportReq), value);

  for (const reasonId of [1, '1']) {
    const payload = {
      codec: connector.ZlinkStreamCodec.Json,
      payload: new TextEncoder().encode(JSON.stringify({ reasonId })),
      messageType: ProfileReportReq
    };
    assert.throws(() => json.zlinkStreamJsonCodec.decode(payload, ProfileReportReq));
  }
});

test('stream connector json codec encodes and decodes json payloads', () => {
  const payload = json.toJson({ ready: true });

  assert.equal(payload.codec, connector.ZlinkStreamCodec.Json);
  assert.deepEqual(json.fromJson(payload), { ready: true });
  assert.throws(
    () => json.fromJson({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }),
    /not Json/
  );
});

test('stream connector json codec rejects prototype mutation keys from wire payloads', () => {
  const payload = {
    codec: connector.ZlinkStreamCodec.Json,
    payload: new TextEncoder().encode('{"safe":true,"__proto__":{"polluted":true}}')
  };

  assert.throws(() => json.fromJson(payload), /not allowed/);
  assert.equal({}.polluted, undefined);
});

test('stream connector json codec writes json payload frame through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  instance.send(new Ready()).metadata('trace', 'json-1').submit();

  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.codec, connector.ZlinkStreamCodec.Json);
  assert.equal(header.name, 'Ready');
  assert.equal(header.metadata.get('trace'), 'json-1');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { ready: true });
  await instance.close();
});

test('typed connector send preserves integer boundaries in the transmitted frame', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  await instance.connect();
  await instance
    .send({
      min: -(1n << 63n),
      max: (1n << 63n) - 1n,
      unsigned: (1n << 64n) - 1n,
      small: 2147483647
    })
    .packetName('Counters')
    .submit();
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), {
    min: '-9223372036854775808',
    max: '9223372036854775807',
    unsigned: '18446744073709551615',
    small: 2147483647
  });
  await instance.close();
});

// Spec stream-connector 32 section 7: close does not wait for the frame being
// written; its Send fails with Disconnected.
test('stream connector close does not wait for a one-way send write', async () => {
  const transportFactory = new MemoryTransportFactory();
  transportFactory.connection.delayWrites = true;
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const sending = instance.send(new Ready()).packetName('Ready').submit();
  await instance.close();

  await assert.rejects(
    sending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.Disconnected
  );
  assert.equal(transportFactory.connection.frames.length, 0);
  assert.equal(transportFactory.connection.closed, true);
  transportFactory.connection.releaseWrites();
});

test('stream connector json codec decodes reply payload through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance.request(new Join()).timeout(1000).submit();

  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(
    transportFactory.connection.frames[0]
  );
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Response,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
        requestSeq: requestHeader.requestSeq,
        name: 'Join',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"accepted":true}')
    )
  );

  await instance.dispatch();
  assert.deepEqual(await pending, { accepted: true });
  await instance.close();
});

test('stream connector json codec decodes plain-object request replies through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance.request({ join: true }).packetName('Join').timeout(1000).submit();

  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(
    transportFactory.connection.frames[0]
  );
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Response,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
        requestSeq: requestHeader.requestSeq,
        name: 'Join',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"accepted":true}')
    )
  );

  await instance.dispatch();
  assert.deepEqual(await pending, { accepted: true });
  await instance.close();
});

test('stream connector json codec dispatches typed payloads through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const received = [];

  instance.on('Notice', (message) => {
    received.push(json.fromJson(message.payload));
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"notice":1}')
    )
  );

  await instance.dispatch();
  assert.deepEqual(received, [{ notice: 1 }]);
  await instance.close();
});

test('stream connector json codec wait resolves matching typed payload', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .waitFor('Notice')
    .timeout(1000)
    .where((message) => message.payload.notice === 2)
    .submit();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"notice":1}')
    )
  );
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"notice":2}')
    )
  );

  await instance.dispatch();
  await instance.dispatch();

  assert.deepEqual((await pending).payload, { notice: 2 });
  await instance.close();
});

test('stream connector wait consumes a matching message received before registration', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'GameStarted',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"round":7}')
    )
  );
  await instance.dispatch();

  const received = await instance.waitFor('GameStarted').timeout(1000).submit();

  assert.deepEqual(received.payload, { round: 7 });
  await instance.close();
});

test('stream connector json codec wait uses connector request timeout by default', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    requestTimeoutMs: 1000,
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .waitFor('Notice')
    .where((message) => message.payload.notice === 3)
    .submit();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"notice":3}')
    )
  );

  await instance.dispatch();

  assert.deepEqual((await pending).payload, { notice: 3 });
  await instance.close();
});

class MemoryTransportFactory {
  constructor() {
    this.connection = new MemoryConnection();
  }

  async connect() {
    return this.connection;
  }
}

class Ready {
  constructor() {
    this.ready = true;
  }
}

class Join {
  constructor() {
    this.join = true;
  }
}

class MemoryConnection {
  constructor() {
    this.frames = [];
    this.inbound = [];
    this.closed = false;
    this.delayWrites = false;
    this.writeResolvers = [];
  }

  async write(frame) {
    if (this.delayWrites) {
      await new Promise((resolve) => this.writeResolvers.push(resolve));
    }
    this.frames.push(frame);
  }

  async read() {
    return this.inbound.shift();
  }

  pushFrame(frame) {
    this.inbound.push(frame);
  }

  releaseWrites() {
    const resolvers = this.writeResolvers.splice(0);
    for (const resolve of resolvers) {
      resolve();
    }
  }

  async close() {
    this.closed = true;
  }
}
