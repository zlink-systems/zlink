const assert = require('node:assert/strict');
const test = require('node:test');

const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');
const json = connector;

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
    transportFactory,
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

test('stream connector close drains submitted one-way send writes', async () => {
  const transportFactory = new MemoryTransportFactory();
  transportFactory.connection.delayWrites = true;
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
  });

  await instance.connect();
  instance.send(new Ready()).packetName('Ready').submit();
  const closing = instance.close();

  assert.equal(transportFactory.connection.frames.length, 0);
  transportFactory.connection.releaseWrites();
  await closing;

  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.name, 'Ready');
  assert.equal(transportFactory.connection.closed, true);
});

test('stream connector json codec decodes reply payload through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
  });

  await instance.connect();
  const pending = instance.request(new Join()).timeout(1000).submit();

  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Response,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq: requestHeader.requestSeq,
      name: 'Join',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"accepted":true}')
  ));

  await instance.dispatch();
  assert.deepEqual(await pending, { accepted: true });
  await instance.close();
});

test('stream connector json codec decodes plain-object request replies through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
  });

  await instance.connect();
  const pending = instance
    .request({ join: true })
    .packetName('Join')
    .timeout(1000)
    .submit();

  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Response,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq: requestHeader.requestSeq,
      name: 'Join',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"accepted":true}')
  ));

  await instance.dispatch();
  assert.deepEqual(await pending, { accepted: true });
  await instance.close();
});

test('stream connector json codec dispatches typed payloads through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
  });
  const received = [];

  instance.on('Notice', (message) => {
    received.push(json.fromJson(message.payload));
  });

  await instance.connect();
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: 'Notice',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"notice":1}')
  ));

  await instance.dispatch();
  assert.deepEqual(received, [{ notice: 1 }]);
  await instance.close();
});

test('stream connector json codec wait resolves matching typed payload', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
  });

  await instance.connect();
  const pending = instance
    .waitFor('Notice')
    .timeout(1000)
    .where((message) => message.payload.notice === 2)
    .submit();
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: 'Notice',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"notice":1}')
  ));
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: 'Notice',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"notice":2}')
  ));

  await instance.dispatch();
  await instance.dispatch();

  assert.deepEqual((await pending).payload, { notice: 2 });
  await instance.close();
});

test('stream connector wait consumes a matching message received before registration', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
  });

  await instance.connect();
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: 'GameStarted',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"round":7}')
  ));
  await instance.dispatch();

  const received = await instance
    .waitFor('GameStarted')
    .timeout(1000)
    .submit();

  assert.deepEqual(received.payload, { round: 7 });
  await instance.close();
});

test('stream connector json codec wait uses connector request timeout by default', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    requestTimeoutMs: 1000,
    transportFactory,
  });

  await instance.connect();
  const pending = instance
    .waitFor('Notice')
    .where((message) => message.payload.notice === 3)
    .submit();
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: 'Notice',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"notice":3}')
  ));

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
