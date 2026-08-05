const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const test = require('node:test');

const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');
const msgpack = require('../../packages/framework-codec-msgpack/dist');
const protobuf = require('../../packages/framework-codec-protobuf/dist');
const protobufFramework = require('../../packages/framework-codec-protobuf/dist/server/framework.cjs');

test('stream connector messagepack codec encodes and decodes payloads', () => {
  const payload = msgpack.toMsgPack({ ready: true });

  assert.equal(payload.codec, connector.ZlinkStreamCodec.MessagePack);
  assert.notEqual(Buffer.from(payload.payload).toString('utf8'), '{"ready":true}');
  assert.deepEqual(msgpack.fromMsgPack(payload), { ready: true });
  assert.throws(
    () => msgpack.fromMsgPack({ codec: connector.ZlinkStreamCodec.Json, payload: new Uint8Array() }),
    /not MessagePack/
  );
});

test('stream connector messagepack codec decodes replies through connector', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://browser.test/stream',
    transportFactory,
    codec: msgpack.zlinkStreamMessagePackCodec
  });

  try {
    await instance.connect();
    const pending = instance.request(new Join()).timeout(1000).submit();

    const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
    const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
    transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Response,
        codec: connector.ZlinkStreamCodec.MessagePack,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
        requestSeq: requestHeader.requestSeq,
        name: 'Join',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      msgpack.toMsgPack({ accepted: true }).payload
    ));

    await instance.dispatch();
    assert.deepEqual(await pending, { accepted: true });
  } finally {
    // A connected connector holds its heartbeat interval until close().
    await instance.close();
  }
});

test('stream connector protobuf codec uses supplied protobuf type', () => {
  const type = createLengthPrefixedJsonType();
  const payload = protobuf.toProto({ ready: true }, type);

  assert.equal(payload.codec, connector.ZlinkStreamCodec.Protobuf);
  assert.deepEqual(protobuf.fromProto(payload, type), { ready: true });
  assert.throws(
    () => protobuf.fromProto({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }, type),
    /not Protobuf/
  );
});

test('stream connector protobuf codec dispatches typed payloads through connector', async () => {
  const type = createLengthPrefixedJsonType();
  const transportFactory = new MemoryTransportFactory();
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://browser.test/stream',
    transportFactory,
    codec: protobuf.createZlinkStreamProtobufCodec(type)
  });
  const received = [];

  instance.on('Notice', (message) => {
    received.push(message.payload);
  });

  try {
    await instance.connect();
    transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Protobuf,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      protobuf.toProto({ notice: 1 }, type).payload
    ));

    await instance.dispatch();
    assert.deepEqual(received, [{ notice: 1 }]);
  } finally {
    // A connected connector holds its heartbeat interval until close().
    await instance.close();
  }
});

test('protobuf envelope extension registers the schema-backed envelope serializer', () => {
  const encodedBytes = Uint8Array.from([7, 8, 9]);
  const calls = [];
  let serializer;
  const extension = protobufFramework.createZlinkProtobufEnvelopeCodec({
    encode(value, context) {
      calls.push({ direction: 'encode', value, context });
      return {
        codec: connector.ZlinkStreamCodec.Protobuf,
        payload: encodedBytes
      };
    },
    decode(payload) {
      calls.push({ direction: 'decode', payload });
      return { decoded: true };
    }
  });

  extension.register({
    addSerializer(contentType, candidate) {
      assert.equal(contentType, protobuf.ZLINK_PROTOBUF_CONTENT_TYPE);
      serializer = candidate;
    },
    addStreamCodec(contentType, candidate) {
      assert.equal(contentType, protobuf.ZLINK_PROTOBUF_CONTENT_TYPE);
      assert.equal(candidate, extension);
    }
  });

  const value = new Join();
  const encoded = serializer.serialize(value);
  assert.deepEqual([...encoded.data()], [...encodedBytes]);
  assert.equal(calls[0].direction, 'encode');
  assert.equal(calls[0].value, value);
  assert.equal(calls[0].context, Join);

  assert.deepEqual(serializer.deserialize(encoded), { decoded: true });
  assert.equal(calls[1].direction, 'decode');
  assert.equal(calls[1].payload.codec, connector.ZlinkStreamCodec.Protobuf);
  assert.deepEqual([...calls[1].payload.payload], [...encodedBytes]);
});

test('generated Bingo browser codec is deterministic and round-trips without filesystem lookup', async () => {
  const root = path.resolve(__dirname, '../..');
  const sample = path.join(root, 'samples/Bingo.Ts');
  const generated = path.join(sample, 'Shared/Contracts/bingo-messages.generated.ts');
  const generator = path.join(sample, 'scripts/generate-protobuf-types.js');
  childProcess.execFileSync(process.execPath, [generator], { cwd: sample });
  const first = crypto.createHash('sha256').update(fs.readFileSync(generated)).digest('hex');
  childProcess.execFileSync(process.execPath, [generator], { cwd: sample });
  const second = crypto.createHash('sha256').update(fs.readFileSync(generated)).digest('hex');
  assert.equal(second, first);

  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-bingo-codec-'));
  try {
    const entry = path.join(temporary, 'entry.ts');
    const output = path.join(temporary, 'entry.mjs');
    fs.writeFileSync(entry, [
      `export { bingoProtobuf } from ${JSON.stringify(path.join(sample, 'Shared/Contracts/protobuf-browser-codec.ts'))};`,
      `export { AuthenticateReq, AuthenticatePlayerRes } from ${JSON.stringify(generated)};`
    ].join('\n'));
    childProcess.execFileSync(path.join(root, 'node_modules/.bin/esbuild'), [
      entry,
      '--bundle',
      '--platform=browser',
      '--format=esm',
      `--outfile=${output}`
    ], { cwd: root });
    const browser = await import(`${pathToFileURL(output).href}?v=${Date.now()}`);
    const authenticate = new browser.AuthenticateReq({ accessToken: 'player-1' });
    const encoded = browser.bingoProtobuf.encode(authenticate, browser.AuthenticateReq);
    const decoded = browser.bingoProtobuf.decode(encoded);
    assert.equal(decoded.constructor, browser.AuthenticateReq);
    assert.equal(decoded.accessToken, 'player-1');

    const player = new browser.AuthenticatePlayerRes({
      accepted: true,
      actorId: 'player-1',
      displayName: 'Player One',
      reason: null
    });
    const playerEncoded = browser.bingoProtobuf.encode(player, browser.AuthenticatePlayerRes);
    const playerDecoded = browser.bingoProtobuf.decode(playerEncoded);
    assert.equal(playerDecoded.constructor, browser.AuthenticatePlayerRes);
    assert.equal(playerDecoded.actorId, 'player-1');
    assert.equal(playerDecoded.displayName, 'Player One');
    assert.equal(playerDecoded.reason, null);

    const bundle = fs.readFileSync(output, 'utf8');
    assert.doesNotMatch(bundle, /protoPath|loadSync|node:fs|__dirname/);
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
});

class MemoryTransportFactory {
  constructor() {
    this.connection = new MemoryConnection();
  }

  async connect() {
    return this.connection;
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
  }

  async write(frame) {
    this.frames.push(frame);
  }

  async read() {
    return this.inbound.shift();
  }

  pushFrame(frame) {
    this.inbound.push(frame);
  }

  async close() {}
}

function createLengthPrefixedJsonType() {
  return {
    encode(value) {
      const json = new TextEncoder().encode(JSON.stringify(value));
      return {
        finish() {
          const bytes = new Uint8Array(4 + json.length);
          bytes[0] = (json.length >>> 24) & 0xff;
          bytes[1] = (json.length >>> 16) & 0xff;
          bytes[2] = (json.length >>> 8) & 0xff;
          bytes[3] = json.length & 0xff;
          bytes.set(json, 4);
          return bytes;
        }
      };
    },
    decode(bytes) {
      const length = bytes[0] * 0x1000000 + ((bytes[1] << 16) | (bytes[2] << 8) | bytes[3]);
      return JSON.parse(new TextDecoder().decode(bytes.subarray(4, 4 + length)));
    }
  };
}
