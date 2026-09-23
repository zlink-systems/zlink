const assert = require('node:assert/strict');
const test = require('node:test');

const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');

// A connected connector owns a heartbeat interval that keeps the Node event
// loop alive until close() stops it (ZlinkStreamConnectorLifecycle.closeOnce).
// Every scenario below opens connectors, so the file tracks them and closes
// them once the whole suite has finished asserting.
const openedConnectors = [];

function createStreamConnector(options) {
  const instance = connector.zlinkStreamConnectorFactory.create(options);
  openedConnectors.push(instance);
  return instance;
}

test.after(async () => {
  for (const instance of openedConnectors.splice(0).reverse()) {
    // A scenario may already have closed the connector or driven its transport
    // into a failure that close() re-reports; neither is a cleanup failure.
    await instance.close().catch(() => undefined);
  }
});

test('stream connector exposes dotnet-shaped enums factory and default options', () => {
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: new MemoryTransportFactory()
  });

  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Created);
  assert.equal(instance.options.transport, connector.ZlinkStreamTransport.WebSocket);
  assert.equal(instance.options.connectTimeoutMs, 5000);
  assert.equal(instance.options.requestTimeoutMs, 30000);
  assert.equal(instance.options.heartbeat.enabled, true);
  assert.equal(instance.options.reconnect.maxAttempts, 3);
  assert.equal(instance.options.maxReceivePayloadSize, 64 * 1024);
});

test('stream header and frame codec follow dotnet binary layout', () => {
  const metadata = connector.ZlinkStreamMetadataMap.empty.with('trace', 'abc');
  const header = {
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 7n,
    name: 'Join',
    metadata
  };

  const encodedHeader = protocolCodecs.ZlinkStreamHeaderCodec.encode(header);
  assert.deepEqual([...encodedHeader.slice(0, 4)], [0xf2, 2, 1, 3]);
  assert.equal(encodedHeader[12], 4);

  const decodedHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(encodedHeader);
  assert.equal(decodedHeader.kind, connector.ZlinkStreamMessageKind.Request);
  assert.equal(decodedHeader.codec, connector.ZlinkStreamCodec.Json);
  assert.equal(decodedHeader.requestSeq, 7n);
  assert.equal(decodedHeader.name, 'Join');
  assert.equal(decodedHeader.metadata.get('trace'), 'abc');

  const frame = protocolCodecs.ZlinkStreamFrameCodec.encode(encodedHeader, new Uint8Array([1, 2, 3]));
  assert.equal((frame[0] << 8) | frame[1], encodedHeader.length);
  assert.equal(frame[5], 3);
  const decodedFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(frame);
  assert.deepEqual([...decodedFrame.payload], [1, 2, 3]);
});

test('stream header codec round-trips actor slots and rejects malformed slot fields', () => {
  const encoded = protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.HasActorSlot,
    name: 'ActorPush',
    metadata: connector.ZlinkStreamMetadataMap.empty,
    actorSlot: 0x1234
  });
  const decoded = protocolCodecs.ZlinkStreamHeaderCodec.decode(encoded);
  assert.equal(decoded.actorSlot, 0x1234);
  assert.equal(
    decoded.flags & connector.ZlinkStreamHeaderFlags.HasActorSlot,
    connector.ZlinkStreamHeaderFlags.HasActorSlot
  );

  assert.throws(
    () => protocolCodecs.ZlinkStreamHeaderCodec.decode(encoded.slice(0, -1)),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.FrameDecodeFailed
  );
  assert.throws(
    () => protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Control,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.HasActorSlot,
      name: '$zlink.actor.bound',
      metadata: connector.ZlinkStreamMetadataMap.empty,
      actorSlot: 1
    }),
    /must not contain flags/
  );
});

test('bound actor controls project handles messages and outbound actor slots', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  });
  const events = [];
  instance.onActorBound((actor) => events.push(`bound:${actor.actorId}`));
  instance.onActorUnbound((actor) => events.push(`unbound:${actor.actorId}`));
  await instance.connect();

  transportFactory.connection.pushFrame(actorControlFrame('$zlink.actor.bound', [1, 0, 7, 5, ...new TextEncoder().encode('alice')]));
  await instance.dispatch();
  const actor = instance.actor('alice');
  assert.equal(actor.actorId, 'alice');
  assert.equal(actor.isBound, true);
  assert.deepEqual(instance.actors.map((value) => value.actorId), ['alice']);
  assert.deepEqual(events, ['bound:alice']);

  const messages = [];
  actor.on('ActorPush', (message) => messages.push(message.actorId));
  class ActorTyped { static packetName = 'ActorTypedPush'; }
  actor.on(ActorTyped, (message) => messages.push(`typed:${message.actorId}`));
  transportFactory.connection.pushFrame(sendFrameForActor('ActorPush', 'hello', 7));
  await instance.dispatch();
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasActorSlot,
    name: 'ActorTypedPush',
    metadata: connector.ZlinkStreamMetadataMap.empty,
    actorSlot: 7
  }), new TextEncoder().encode('{"x":1}')));
  await instance.dispatch();
  assert.deepEqual(messages, ['alice', 'typed:alice']);

  await actor
    .send({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array([1]) })
    .packetName('ActorSend')
    .submit();
  const outbound = protocolCodecs.ZlinkStreamFrameCodec.decode(
    transportFactory.connection.frames.at(-1)
  );
  assert.equal(protocolCodecs.ZlinkStreamHeaderCodec.decode(outbound.header).actorSlot, 7);

  const hookEvents = [];
  instance.onRequestSending((ctx) => {
    hookEvents.push(`sending:${ctx.actorId}`);
    ctx.setMetadata('actor-hook', 'yes');
  });
  instance.onReplyReceived((ctx) => hookEvents.push(`reply:${ctx.actorId}:${ctx.succeeded}`));
  const pending = actor
    .request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array([2]) })
    .packetName('ActorRequest')
    .timeout(1000)
    .submitEncoded();
  assert.deepEqual(hookEvents, ['sending:alice']);
  const request = protocolCodecs.ZlinkStreamFrameCodec.decode(
    transportFactory.connection.frames.at(-1)
  );
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(request.header);
  assert.equal(requestHeader.actorSlot, 7);
  assert.equal(requestHeader.metadata.get('actor-hook'), 'yes');
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Response,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq |
          connector.ZlinkStreamHeaderFlags.HasActorSlot,
        requestSeq: requestHeader.requestSeq,
        name: 'ActorRequest',
        metadata: connector.ZlinkStreamMetadataMap.empty,
        actorSlot: 7
      }),
      new Uint8Array([3])
    )
  );
  await instance.dispatch();
  assert.deepEqual([...(await pending).payload], [3]);
  await instance.dispatch();
  assert.deepEqual(hookEvents, ['sending:alice', 'reply:alice:true']);

  transportFactory.connection.pushFrame(actorControlFrame('$zlink.actor.unbound', [1, 0, 7]));
  await instance.dispatch();
  assert.equal(actor.isBound, false);
  assert.equal(instance.actor('alice'), undefined);
  assert.deepEqual(events, ['bound:alice', 'unbound:alice']);
  assert.throws(
    () => actor.send({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
  assert.throws(
    () => actor.request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
});

test('manual transport disconnect dispatches actor unbound before state and disconnected', async () => {
  const connection = new MemoryConnection();
  connection.pushFrame(actorControlFrame('$zlink.actor.bound', [1, 0, 7, 1, 97]));
  let rejectRead;
  const read = connection.read.bind(connection);
  connection.read = async () => {
    const frame = await read();
    if (frame !== undefined) return frame;
    return await new Promise((_resolve, reject) => { rejectRead = reject; });
  };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  });
  const lifecycle = [];
  instance.onActorUnbound((actor) => lifecycle.push(actor.actorId));
  instance.onConnectionStateChanged((change) => lifecycle.push(change.current));
  instance.onDisconnected(() => lifecycle.push('disconnected'));
  await instance.connect();
  await waitFor(() => instance.actors.length === 1, 1000);
  await instance.dispatch();
  lifecycle.length = 0;

  rejectRead(new Error('transport failed'));
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  assert.deepEqual(lifecycle, []);
  await instance.dispatch();
  assert.deepEqual(lifecycle, [
    'a',
    connector.ZlinkStreamConnectionState.Disconnected,
    'disconnected'
  ]);
});

test('unknown actor slots fail decoding and close the connection', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    reconnect: { enabled: false }
  });
  const errors = [];
  const lifecycle = [];
  instance.onErrorReceived((error) => errors.push(error.code));
  instance.onActorUnbound((actor) => lifecycle.push(`unbound:${actor.actorId}`));
  instance.onDisconnected(() => lifecycle.push('disconnected'));
  await instance.connect();
  transportFactory.connection.pushFrame(
    actorControlFrame('$zlink.actor.bound', [1, 0, 1, 1, 97])
  );
  transportFactory.connection.pushFrame(
    actorControlFrame('$zlink.actor.bound', [1, 0, 2, 1, 98])
  );
  await instance.dispatch();
  transportFactory.connection.pushFrame(sendFrameForActor('UnknownActor', 'bad', 99));
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  await waitFor(() => lifecycle.includes('disconnected'), 1000);
  assert.equal(transportFactory.connection.closed, true);
  assert.ok(errors.includes(connector.ZlinkStreamErrorCode.FrameDecodeFailed));
  assert.deepEqual(lifecycle, ['unbound:a', 'unbound:b', 'disconnected']);
});

test('invalid actor lifecycle controls fail decoding and close the connection', async () => {
  const invalidControls = [
    { name: 'duplicate slot', setup: [[1, 0, 7, 1, 97]], invalid: [1, 0, 7, 1, 98] },
    { name: 'duplicate actor id', setup: [[1, 0, 7, 1, 97]], invalid: [1, 0, 8, 1, 97] },
    { name: 'unknown unbound slot', setup: [], invalid: [1, 0, 9], packet: '$zlink.actor.unbound' },
    { name: 'malformed bound payload', setup: [], invalid: [1, 0, 7, 2, 97] }
  ];

  for (const scenario of invalidControls) {
    const transportFactory = new MemoryTransportFactory();
    const instance = createStreamConnector({
      endpoint: 'ws://127.0.0.1:19000',
      transportFactory,
      heartbeat: { enabled: false },
      reconnect: { enabled: false }
    });
    await instance.connect();
    for (const payload of scenario.setup) {
      transportFactory.connection.pushFrame(actorControlFrame('$zlink.actor.bound', payload));
      await instance.dispatch();
    }
    transportFactory.connection.pushFrame(
      actorControlFrame(scenario.packet ?? '$zlink.actor.bound', scenario.invalid)
    );
    await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
    assert.equal(transportFactory.connection.closed, true, scenario.name);
  }
});

test('stream connector rejects outbound metadata above the fixed 1024-byte limit', async () => {
  assert.doesNotThrow(() => protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.HasMetadata,
    name: 'MetadataLimit',
    metadata: connector.ZlinkStreamMetadataMap.empty.with('k', 'x'.repeat(1019))
  }));

  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
    .packetName('MetadataLimit')
    .metadata('large', 'x'.repeat(1024))
    .timeout(1000)
    .submitEncoded();

  await assert.rejects(
    () => pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
  assert.equal(transportFactory.connection.frames.length, 0);
});

test('stream connector send builder writes a dotnet-compatible send frame once', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  await instance
    .send({
      codec: connector.ZlinkStreamCodec.Json,
      payload: new TextEncoder().encode('{"ok":true}')
    })
    .packetName('Ready')
    .metadata('trace', 'send-1')
    .submit();

  assert.equal(transportFactory.connection.frames.length, 1);
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(header.name, 'Ready');
  assert.equal(header.metadata.get('trace'), 'send-1');

  assert.throws(
    () => instance.send({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }).packetName('$zlink.bad'),
    /reserved zlink prefix/
  );
});

test('stream connector disconnected send fails before transport write', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await assert.rejects(() => instance.send({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: new TextEncoder().encode('b')
  }).packetName('h').submit());
  assert.equal(transportFactory.connection.frames.length, 0);
});

test('stream connector send and request enforce payload limit before transport write', async () => {
  const sendTransportFactory = new MemoryTransportFactory();
  const sendInstance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: sendTransportFactory,
    maxSendPayloadSize: 1
  });
  await sendInstance.connect();

  await assert.rejects(
    () => sendInstance.send({
      codec: connector.ZlinkStreamCodec.Raw,
      payload: new TextEncoder().encode('bb')
    }).packetName('h').submit(),
    // stream-connector 32 §4.7/§9: a send-limit violation fails before the
    // transport write as ValidationFailed. FrameTooLarge names the receive
    // bound only and must never appear on the send path.
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
  assert.equal(sendTransportFactory.connection.frames.length, 0);

  const requestTransportFactory = new MemoryTransportFactory();
  const requestInstance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: requestTransportFactory,
    maxSendPayloadSize: 1
  });
  await requestInstance.connect();

  await assert.rejects(
    () => requestInstance.request({
      codec: connector.ZlinkStreamCodec.Raw,
      payload: new TextEncoder().encode('bb')
    }).packetName('h').timeout(1000).submitEncoded(),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
  assert.equal(requestTransportFactory.connection.frames.length, 0);
  assert.equal(requestInstance.pendingDispatchCount, 0);
});

test('stream frame codec reports validationFailed over the send limit and succeeds at the limit', () => {
  // stream-connector 32 §9: ValidationFailed covers "송신 payload 한도 초과"
  // (send payload over limit); FrameTooLarge names the receive bound only.
  // §4.7 line 228 requires this to fail before the transport write.
  const header = new Uint8Array([0x01]);
  const maxSendPayloadSize = 4;

  const overLimitPayload = new Uint8Array(maxSendPayloadSize + 1);
  assert.throws(
    () => protocolCodecs.ZlinkStreamFrameCodec.encode(header, overLimitPayload, maxSendPayloadSize),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );

  const atLimitPayload = new Uint8Array(maxSendPayloadSize);
  assert.doesNotThrow(
    () => protocolCodecs.ZlinkStreamFrameCodec.encode(header, atLimitPayload, maxSendPayloadSize)
  );
});

test('stream connector default compression uses LZ4 before transport write', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const body = new TextEncoder().encode('body');
  await instance.connect();

  await instance.send({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: body
  }).packetName('Compressed').compress().submit();

  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal((header.flags & connector.ZlinkStreamHeaderFlags.PayloadCompressed) !== 0, true);
  assert.deepEqual([...unpickleLz4(frame.payload)], [...body]);
});

test('stream connector disabled compression rejects compressed sends before transport write', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    compression: connector.ZlinkStreamCompression.None
  });
  await instance.connect();

  await assert.rejects(() => instance.send({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: new TextEncoder().encode('body')
  }).packetName('Compressed').compress().submit());
  assert.equal(transportFactory.connection.frames.length, 0);
});

test('stream connector compressed sends write dotnet LZ4-pickled payloads', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    compression: connector.ZlinkStreamCompression.Lz4
  });
  const body = new TextEncoder().encode('compressed-body');
  await instance.connect();

  await instance.send({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: body
  }).packetName('Compressed').compress().submit();

  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal((header.flags & connector.ZlinkStreamHeaderFlags.PayloadCompressed) !== 0, true);
  assert.deepEqual([...unpickleLz4(frame.payload)], [...body]);
});

test('stream connector custom compression codec handles outbound and inbound payloads', async () => {
  const marker = 0x7a;
  const compressionCodec = {
    compress(payload) {
      const compressed = new Uint8Array(payload.length + 1);
      compressed[0] = marker;
      compressed.set(payload, 1);
      return compressed;
    },
    decompress(payload, maxDecompressedSize) {
      assert.equal(payload[0], marker);
      const restored = payload.slice(1);
      if (restored.length > maxDecompressedSize) {
        throw new Error('too large');
      }
      return restored;
    }
  };
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    compressionCodec
  });
  const body = new TextEncoder().encode('custom-body');
  const received = [];
  instance.on('CustomNotice', (message) => {
    received.push(new TextDecoder().decode(message.payload.payload));
  });
  await instance.connect();

  await instance.send({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: body
  }).packetName('CustomSend').compress().submit();

  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  assert.equal(frame.payload[0], marker);

  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
        name: 'CustomNotice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      compressionCodec.compress(new TextEncoder().encode('custom-inbound'))
    )
  );

  await instance.dispatch();
  assert.deepEqual(received, ['custom-inbound']);
});

test('stream connector custom decompression result is checked against receive limit', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    maxReceivePayloadSize: 4,
    compressionCodec: {
      compress(payload) {
        return payload;
      },
      decompress(_payload, maxDecompressedSize) {
        return new Uint8Array(maxDecompressedSize + 1);
      }
    }
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error.code);
  });
  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
        name: 'TooLarge',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new Uint8Array([1])
    )
  );

  await instance.dispatch();
  assert.deepEqual(errors, [connector.ZlinkStreamErrorCode.DecompressionFailed]);
});

test('stream connector applies the receive limit to each decoded frame payload', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    maxReceivePayloadSize: 1,
    reconnect: { enabled: false }
  });
  const errors = [];
  let received = false;
  instance.onErrorReceived((error) => { errors.push(error.code); });
  instance.on('TooLarge', () => { received = true; });
  await instance.connect();
  transportFactory.connection.pushFrame(sendFrame('TooLarge', 'bb'));

  await instance.dispatch();
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  await instance.dispatch();

  assert.deepEqual(errors, [connector.ZlinkStreamErrorCode.FrameTooLarge]);
  assert.equal(received, false);
});

test('stream connector request resolves when dispatch reads matching response frame', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .request({
      codec: connector.ZlinkStreamCodec.Json,
      payload: new TextEncoder().encode('{"join":true}')
    })
    .packetName('Join')
    .timeout(1000)
    .submitEncoded();

  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  assert.equal(instance.pendingDispatchCount, 1);
  assert.equal(requestHeader.kind, connector.ZlinkStreamMessageKind.Request);
  assert.equal(requestHeader.requestSeq, 1n);

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
  const reply = await pending;
  assert.equal(reply.codec, connector.ZlinkStreamCodec.Json);
  assert.equal(new TextDecoder().decode(reply.payload), '{"accepted":true}');
  assert.equal(instance.pendingDispatchCount, 0);
});

test('stream connector accepts a legacy response packet name and matches by sequence', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
    .packetName('Join')
    .timeout(1000)
    .submitEncoded();
  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    encodeLegacyNamedReplyHeader({
      kind: connector.ZlinkStreamMessageKind.Response,
      codec: connector.ZlinkStreamCodec.Raw,
      requestSeq: requestHeader.requestSeq,
      name: 'Other'
    }),
    new Uint8Array()
  ));

  await instance.dispatch();
  const reply = await pending;
  assert.equal(reply.codec, connector.ZlinkStreamCodec.Raw);
  assert.equal(reply.payload.length, 0);
});

test('stream connector reports a response whose request sequence has no pending request', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });

  await instance.connect();
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Response,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq: 999n,
      name: 'ExpiredRequest',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new Uint8Array()
  ));

  await instance.dispatch();

  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, connector.ZlinkStreamErrorCode.FrameDecodeFailed);
});

test('stream connector decodes correlated Error JSON without a packet name', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
    .packetName('Join')
    .timeout(1000)
    .submitEncoded();
  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  const responseHeader = protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Error,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: requestHeader.requestSeq,
    name: 'Join',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  assert.equal(responseHeader[12], 0, 'Response and Error must encode name_len = 0');
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    responseHeader,
    new TextEncoder().encode('{"code":"denied","message":"remote failed"}')
  ));

  await instance.dispatch();
  assert.equal(instance.pendingDispatchCount, 0);
  await assert.rejects(
    () => pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.RemoteError
      && error.error.message === 'remote failed'
      && error.error.cause?.code === 'denied'
  );
});

test('stream connector rejects malformed correlated Error JSON', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
    .packetName('Join')
    .timeout(1000)
    .submitEncoded();
  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Error,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq: requestHeader.requestSeq,
      name: 'Join',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('{"code":"denied"}')
  ));

  await instance.dispatch();
  await assert.rejects(
    () => pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.FrameDecodeFailed
  );
});

test('stream connector accepts a legacy correlated Error packet name and matches by sequence', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  const pending = instance
    .request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
    .packetName('Join')
    .timeout(1000)
    .submitEncoded();
  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    encodeLegacyNamedReplyHeader({
      kind: connector.ZlinkStreamMessageKind.Error,
      codec: connector.ZlinkStreamCodec.Json,
      requestSeq: requestHeader.requestSeq,
      name: 'Other'
    }),
    new TextEncoder().encode('{"code":"denied","message":"remote failed"}')
  ));

  await instance.dispatch();
  await assert.rejects(
    () => pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.RemoteError
      && error.error.message === 'remote failed'
      && error.error.cause?.code === 'denied'
  );
});

function encodeLegacyNamedReplyHeader({ kind, codec, requestSeq, name }) {
  const nameBytes = new TextEncoder().encode(name);
  const header = new Uint8Array(13 + nameBytes.length);
  header[0] = 0xf2;
  header[1] = kind;
  header[2] = codec;
  header[3] = connector.ZlinkStreamHeaderFlags.HasRequestSeq;
  new DataView(header.buffer).setBigUint64(4, requestSeq);
  header[12] = nameBytes.length;
  header.set(nameBytes, 13);
  return header;
}

test('stream connector test helpers observe absence and ordered payloads through the received queue', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  await instance.connect();

  await instance.expectNone('Notice').within(5).run();

  const unexpected = instance.expectNone('Notice').within(1000).run();
  const unexpectedRejected = assert.rejects(() => unexpected, (error) =>
    error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed);
  transportFactory.connection.pushFrame(sendFrame('Notice', 'unexpected'));
  await instance.dispatch();
  await unexpectedRejected;

  const sequence = instance.waitForSequence('Notice')
    .expect(() => true)
    .expect(() => true)
    .timeout(1000)
    .run();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'first'));
  await instance.dispatch();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'second'));
  await instance.dispatch();
  // Spec 32 section 10.1: the call answers with messages, so the packet name and
  // the metadata are readable next to the payload.
  const messages = await sequence;
  assert.deepEqual(messages.map((message) => message.name), ['Notice', 'Notice']);
  assert.deepEqual(
    messages.map((message) => new TextDecoder().decode(message.payload.payload)),
    ['first', 'second']
  );

  const outOfOrder = instance.waitForSequence('Notice')
    .expect((message) => new TextDecoder().decode(message.payload.payload) === 'first')
    .timeout(1000)
    .run();
  const outOfOrderRejected = assert.rejects(() => outOfOrder, (error) =>
    error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed);
  transportFactory.connection.pushFrame(sendFrame('Notice', 'wrong'));
  await instance.dispatch();
  await outOfOrderRejected;
  await instance.close();
});

test('zlinkStreamAssert classifies failures and timeouts without pre-started promises', async () => {
  assert.throws(
    () => connector.zlinkStreamAssert.ensure(false, 'required explanation'),
    /required explanation/
  );
  const failure = await connector.zlinkStreamAssert.expectFailure(async () => {
    throw new connector.ZlinkStreamException({
      code: connector.ZlinkStreamErrorCode.RemoteError,
      message: 'remote failure'
    });
  }, connector.ZlinkStreamErrorCode.RemoteError);
  assert.equal(failure.code, connector.ZlinkStreamErrorCode.RemoteError);

  await connector.zlinkStreamAssert.expectTimeout(async () => {
    throw new connector.ZlinkStreamException({
      code: connector.ZlinkStreamErrorCode.RequestTimeout,
      message: 'timed out'
    });
  });
  await assert.rejects(
    () => connector.zlinkStreamAssert.expectTimeout(async () => {
      throw new connector.ZlinkStreamException({
        code: connector.ZlinkStreamErrorCode.RemoteError,
        message: 'not a timeout'
      });
    }),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.RemoteError
  );
});

test('stream connector request resolves compressed response payloads', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    compression: connector.ZlinkStreamCompression.Lz4
  });

  await instance.connect();
  const pending = instance
    .request({
      codec: connector.ZlinkStreamCodec.Raw,
      payload: new TextEncoder().encode('request')
    })
    .packetName('CompressedRequest')
    .timeout(1000)
    .submitEncoded();

  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  const compressedPayload = Uint8Array.from(Buffer.from('40551F41010047504141414141', 'hex'));
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Response,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq | connector.ZlinkStreamHeaderFlags.PayloadCompressed,
        requestSeq: requestHeader.requestSeq,
        name: 'CompressedRequest',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      compressedPayload
    )
  );

  await instance.dispatch();
  const reply = await pending;
  assert.equal(new TextDecoder().decode(reply.payload), 'A'.repeat(96));
});

test('stream connector rejects compressed response payloads above receive limit', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    compression: connector.ZlinkStreamCompression.Lz4,
    maxReceivePayloadSize: 2
  });

  await instance.connect();
  const pending = instance
    .request({
      codec: connector.ZlinkStreamCodec.Raw,
      payload: new TextEncoder().encode('request')
    })
    .packetName('CompressedRequest')
    .timeout(1000)
    .submitEncoded();

  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Response,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq | connector.ZlinkStreamHeaderFlags.PayloadCompressed,
        requestSeq: requestHeader.requestSeq,
        name: 'CompressedRequest',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      Uint8Array.from([0x40, 0x02])
    )
  );

  await instance.dispatch();
  assert.equal(instance.pendingDispatchCount, 0);
  await assert.rejects(
    () => pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.DecompressionFailed
  );
});

test('stream connector dispatch invokes typed handlers for send frames', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const received = [];
  instance.on('Notice', (message) => {
    received.push({
      name: message.name,
      trace: message.metadata.get('trace'),
      payload: new TextDecoder().decode(message.payload.payload)
    });
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.HasMetadata,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty.with('trace', 'handler-1')
      }),
      new TextEncoder().encode('{"notice":1}')
    )
  );

  await instance.dispatch();
  assert.deepEqual(received, [
    {
      name: 'Notice',
      trace: 'handler-1',
      payload: '{"notice":1}'
    }
  ]);
});

test('stream connector dispatch decompresses send frames for handlers', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    compression: connector.ZlinkStreamCompression.Lz4
  });
  const received = [];
  instance.on('CompressedNotice', (message) => {
    received.push(new TextDecoder().decode(message.payload.payload));
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
        name: 'CompressedNotice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      Uint8Array.from(Buffer.from('40551F41010047504141414141', 'hex'))
    )
  );

  await instance.dispatch();
  assert.deepEqual(received, ['A'.repeat(96)]);
});

test('stream connector publishes decompression error for compressed frames when compression is disabled', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    compression: connector.ZlinkStreamCompression.None
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });
  instance.on('CompressedNotice', () => {});

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
        name: 'CompressedNotice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      Uint8Array.from(Buffer.from('40551F41010047504141414141', 'hex'))
    )
  );

  await instance.dispatch();
  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, connector.ZlinkStreamErrorCode.DecompressionFailed);
  assert.match(errors[0].message, /compression codec/i);
});

test('stream connector dispatch publishes decode errors for invalid header frames', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { enabled: false }
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      new TextEncoder().encode('invalid-header'),
      new TextEncoder().encode('payload')
    )
  );

  await instance.dispatch();
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  await instance.dispatch();

  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, connector.ZlinkStreamErrorCode.FrameDecodeFailed);
});

test('stream connector dispatch publishes uncorrelated remote error packets', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Error,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'RemoteError',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"code":"denied","message":"remote failed"}')
    )
  );

  await instance.dispatch();

  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, connector.ZlinkStreamErrorCode.RemoteError);
  assert.equal(errors[0].message, 'remote failed');
  assert.deepEqual(errors[0].cause, { code: 'denied', message: 'remote failed' });
});

test('stream connector publishes remote errors whose request sequence has no pending request', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Error,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
        requestSeq: 999n,
        name: 'ExpiredRequest',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"code":"expired","message":"request no longer exists"}')
    )
  );

  await instance.dispatch();

  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, connector.ZlinkStreamErrorCode.RemoteError);
  assert.equal(errors[0].message, 'request no longer exists');
  assert.deepEqual(errors[0].cause, { code: 'expired', message: 'request no longer exists' });
});

test('stream connector reports malformed Error JSON with no pending request as a decode failure', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Error,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
        requestSeq: 999n,
        name: 'ExpiredRequest',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"code":7,"message":"invalid code type"}')
    )
  );

  await instance.dispatch();

  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, connector.ZlinkStreamErrorCode.FrameDecodeFailed);
});

test('stream connector dispatch publishes user callback failures without throwing', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });
  instance.on('Notice', () => {
    throw new Error('handler failed');
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

  assert.equal(errors.length, 1);
  assert.equal(errors[0].code, connector.ZlinkStreamErrorCode.UserCallbackFailed);
});

test('stream connector rejects reserved packet names for user handlers', () => {
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: new MemoryTransportFactory()
  });

  assert.throws(
    () => instance.on('$zlink.user', () => {}),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
});

test('DRAIN-018 session-closing exposes ServerDrain before disconnected callback', async () => {
  const payload = Uint8Array.from([1, 4, 0, 0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Control,
    codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'session-closing',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  const frame = protocolCodecs.ZlinkStreamFrameCodec.encode(header, payload);
  let delivered = false;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:7998',
    reconnect: { enabled: false },
    heartbeat: { enabled: false },
    transportFactory: {
      async connect() {
        return {
          async write() {},
          async read() {
            if (delivered) return undefined;
            delivered = true;
            return frame;
          },
          async close() {}
        };
      }
    }
  });
  let observed;
  instance.onDisconnected(() => { observed = instance.closeReason; });
  await instance.connect();
  await instance.dispatch();
  assert.equal(instance.closeReason, 'ServerDrain');
  assert.equal(observed, 'ServerDrain');
});

test('stream connector dispatch replies to heartbeat ping control frames with pong', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Control,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: '$zlink.heartbeat.ping',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new Uint8Array()
    )
  );

  await instance.dispatch();
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Control);
  assert.equal(header.name, '$zlink.heartbeat.pong');
});

test('stream connector validates endpoint and lifecycle options like dotnet transport factory', () => {
  assert.throws(
    () => createStreamConnector({ endpoint: 'http://127.0.0.1:1' }),
    /supports only ws:\/\/ and wss:\/\//
  );
  assert.throws(
    () => createStreamConnector({ endpoint: 'tcp://127.0.0.1:1' }),
    /supports only ws:\/\/ and wss:\/\//
  );
  assert.throws(
    () => createStreamConnector({ endpoint: 'ws://127.0.0.1:1', heartbeat: { intervalMs: 5, timeoutMs: 5 } }),
    /Heartbeat timeout must be greater/
  );
  assert.throws(
    () => createStreamConnector({ endpoint: 'ws://127.0.0.1:1', reconnect: { backoffFactor: 0.5 } }),
    /BackoffFactor/
  );
  assert.throws(
    () => createStreamConnector({ endpoint: 'ws://127.0.0.1:1', maxReceivePayloadSize: 0 }),
    /MaxReceivePayloadSize/
  );
});

test('stream connector heartbeat loop sends ping control frames after connect', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    heartbeat: { intervalMs: 1, timeoutMs: 1000 }
  });

  await instance.connect();
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(await transportFactory.connection.nextWrite());
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Control);
  assert.equal(header.name, '$zlink.heartbeat.ping');
  await instance.close();
});

test('stream connector connect retries through reconnect options before succeeding', async () => {
  const transportFactory = new FlakyTransportFactory(1);
  const states = [];
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });
  instance.onConnectionStateChanged((change) => {
    states.push(change.current);
  });

  await instance.connect();

  assert.equal(transportFactory.attempts, 2);
  assert.equal(instance.isConnected, true);
  assert.ok(states.includes(connector.ZlinkStreamConnectionState.Reconnecting));
  await instance.close();
});

test('stream connector reports exhausted reconnect state transitions', async () => {
  const transportFactory = new FlakyTransportFactory(3);
  const states = [];
  const errors = [];
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });
  instance.onConnectionStateChanged((change) => states.push(change.current));
  instance.onErrorReceived((error) => errors.push(error.code));

  await assert.rejects(() => instance.connect(), /Connect failed/);

  assert.equal(transportFactory.attempts, 2);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Disconnected);
  assert.deepEqual(states, [
    connector.ZlinkStreamConnectionState.Connecting,
    connector.ZlinkStreamConnectionState.Reconnecting,
    connector.ZlinkStreamConnectionState.Disconnected
  ]);
  assert.deepEqual(errors, [
    connector.ZlinkStreamErrorCode.ConnectTimeout,
    connector.ZlinkStreamErrorCode.ConnectTimeout
  ]);
});

test('stream connector close publishes closed state before one disconnected callback', async () => {
  const events = [];
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    heartbeat: { enabled: false }
  });
  instance.onConnectionStateChanged((change) => events.push(`state:${change.current}`));
  instance.onDisconnected(() => events.push('disconnected'));

  await instance.connect();
  await instance.close();
  await instance.close();
  await instance.dispatch();

  assert.equal(transportFactory.connection.closed, true);
  assert.deepEqual(events.slice(-2), [
    `state:${connector.ZlinkStreamConnectionState.Closed}`,
    'disconnected'
  ]);
  assert.equal(events.filter((event) => event === 'disconnected').length, 1);
});

test('stream connector close does not wait for a connection state handler', async () => {
  const events = [];
  let stateHandlerEntered = false;
  let releaseStateHandler;
  const stateHandler = new Promise((resolve) => { releaseStateHandler = resolve; });
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: new MemoryTransportFactory(),
    heartbeat: { enabled: false }
  });
  instance.onConnectionStateChanged(async (change) => {
    if (change.current === connector.ZlinkStreamConnectionState.Closed) {
      stateHandlerEntered = true;
      events.push('state:Closed');
      await stateHandler;
    }
  });
  instance.onDisconnected(() => events.push('disconnected'));

  await instance.connect();
  const closing = instance.close();
  try {
    await withTimeout(closing, 2000, 'close with a pending connection state handler');
    await withTimeout(instance.dispatch(), 2000, 'dispatch with a pending connection state handler');
  } finally {
    releaseStateHandler();
    await closing;
  }

  assert.equal(stateHandlerEntered, true);
  assert.deepEqual(events, ['state:Closed', 'disconnected']);
});

test('stream connector exhausted reconnect does not wait for a connection state handler', async () => {
  const events = [];
  let stateHandlerEntered = false;
  let releaseStateHandler;
  const stateHandler = new Promise((resolve) => { releaseStateHandler = resolve; });
  const transportFactory = new FlakyTransportFactory(5);
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });
  instance.onConnectionStateChanged(async (change) => {
    if (change.current === connector.ZlinkStreamConnectionState.Disconnected) {
      stateHandlerEntered = true;
      events.push('state:Disconnected');
      await stateHandler;
    }
  });
  instance.onDisconnected(() => events.push('disconnected'));

  const connecting = instance.connect();
  try {
    await withTimeout(assert.rejects(connecting, /Connect failed/), 2000, 'exhausted reconnect');
  } finally {
    releaseStateHandler();
    await connecting.catch(() => undefined);
  }

  assert.equal(stateHandlerEntered, true);
  assert.equal(transportFactory.attempts, 2);
  assert.deepEqual(events, ['state:Disconnected', 'disconnected']);
});

test('stream connector transport failure does not wait for a connection state handler', async () => {
  const events = [];
  let stateHandlerEntered = false;
  let releaseStateHandler;
  const stateHandler = new Promise((resolve) => { releaseStateHandler = resolve; });
  const connection = new MemoryConnection();
  let rejectRead;
  connection.read = () => new Promise((_resolve, reject) => { rejectRead = reject; });
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  instance.onConnectionStateChanged(async (change) => {
    if (change.current === connector.ZlinkStreamConnectionState.Disconnected) {
      stateHandlerEntered = true;
      events.push('state:Disconnected');
      await stateHandler;
    }
  });
  instance.onDisconnected(() => events.push('disconnected'));

  await instance.connect();
  rejectRead(new Error('transport failed'));
  try {
    await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 2000);
    await withTimeout(instance.dispatch(), 2500, 'transport failure dispatch');
  } finally {
    releaseStateHandler();
  }

  assert.deepEqual(events, ['state:Disconnected', 'disconnected']);
});

test('stream connector shares concurrent connect and closes a connection that completes after close starts', async () => {
  let connectCalls = 0;
  let resolveConnection;
  const connectionReady = new Promise((resolve) => { resolveConnection = resolve; });
  const connection = new MemoryConnection();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: {
      async connect() {
        connectCalls++;
        return await connectionReady;
      }
    },
    heartbeat: { enabled: false }
  });

  const first = instance.connect();
  const second = instance.connect();
  await waitFor(() => connectCalls === 1, 1000);
  const closing = instance.close();
  resolveConnection(connection);

  await assert.rejects(() => first, /closed while connecting/);
  await assert.rejects(() => second, /closed while connecting/);
  await closing;
  assert.equal(connectCalls, 1);
  assert.equal(connection.closed, true);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Closed);
});

test('stream connector close reports failure to clean up a late connect result', async () => {
  let resolveConnection;
  const connectionReady = new Promise((resolve) => { resolveConnection = resolve; });
  const connection = new MemoryConnection();
  connection.close = async () => { throw new Error('late connection close failed'); };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return await connectionReady; } },
    heartbeat: { enabled: false }
  });

  const connecting = instance.connect();
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Connecting, 1000);
  const closing = instance.close();
  resolveConnection(connection);

  await assert.rejects(() => connecting, /late connection close failed/);
  await assert.rejects(() => closing, /late connection close failed/);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Closed);
});

test('stream connector concurrent close shares cleanup and remains closed when transport close fails', async () => {
  let closeCalls = 0;
  const connection = new MemoryConnection();
  connection.close = async () => {
    closeCalls++;
    throw new Error('transport close failed');
  };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    heartbeat: { enabled: false }
  });
  let disconnectedCalls = 0;
  instance.onDisconnected(async () => {
    disconnectedCalls++;
    throw new Error('user callback failed');
  });
  await instance.connect();

  const first = assert.rejects(() => instance.close(), /transport close failed/);
  const second = assert.rejects(() => instance.close(), /transport close failed/);
  await Promise.all([first, second]);
  await instance.dispatch();

  assert.equal(closeCalls, 1);
  assert.equal(disconnectedCalls, 1);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Closed);
  await instance.close();
});

test('stream connector pong write failure disconnects instead of reporting a decode error', async () => {
  const connection = new MemoryConnection();
  connection.write = async (frame) => {
    const decoded = protocolCodecs.ZlinkStreamFrameCodec.decode(frame);
    const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(decoded.header);
    if (header.name === '$zlink.heartbeat.pong') {
      throw new Error('pong write failed');
    }
  };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  const errors = [];
  instance.onErrorReceived((error) => errors.push(error.code));
  await instance.connect();
  connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Control,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: '$zlink.heartbeat.ping',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new Uint8Array()
  ));

  // The receive loop owns the transport in both dispatch modes, so the failed
  // pong surfaces there as a disconnect and an error event, not as a dispatch
  // rejection (spec stream-connector 32 section 7).
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  await instance.dispatch();
  assert.equal(connection.closed, true);
  assert.deepEqual(errors, [connector.ZlinkStreamErrorCode.SendFailed]);
});

// Spec stream-connector 32 section 7: waitFor, expectNone and waitForSequence are
// not registered callbacks. They observe and consume the packets the receive queue
// has not delivered yet in both dispatch modes, so Manual -- the default -- needs
// no dispatch pump to finish a wait. This scenario never calls dispatch().
test('stream connector manual dispatch mode completes waitFor without a dispatch pump', async () => {
  const connection = new MemoryConnection();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });

  await instance.connect();
  connection.pushFrame(sendFrame('ManualWait', 'waited'));

  const message = await instance.waitFor('ManualWait').timeout(1000).submit();

  assert.equal(new TextDecoder().decode(message.payload.payload), 'waited');
});

// The other half of the same clause: dispatch still owns the registered push
// handlers. The receive loop advancing the transport in Manual must not run them.
test('stream connector manual dispatch mode keeps registered handlers waiting for dispatch', async () => {
  const connection = new MemoryConnection();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  const received = [];
  instance.on('ManualPush', (message) => {
    received.push(new TextDecoder().decode(message.payload.payload));
  });

  await instance.connect();
  connection.pushFrame(sendFrame('ManualPush', 'queued'));
  await waitFor(() => connection.inbound.length === 0, 1000);
  await new Promise((resolve) => setTimeout(resolve, 10));

  assert.deepEqual(received, []);

  await instance.dispatch();

  assert.deepEqual(received, ['queued']);
});

// Immediate keeps running registered handlers straight off the receive path, and
// its wait surfaces read the same queue. Neither needs a dispatch pump.
test('stream connector immediate dispatch mode runs handlers and waits without a dispatch pump', async () => {
  const connection = new MemoryConnection();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  const received = [];
  instance.on('ImmediatePush', (message) => {
    received.push(new TextDecoder().decode(message.payload.payload));
  });

  await instance.connect();
  connection.pushFrame(sendFrame('ImmediatePush', 'pushed'));
  await waitFor(() => received.length === 1, 1000);

  assert.deepEqual(received, ['pushed']);

  connection.pushFrame(sendFrame('ImmediateWait', 'waited'));
  const message = await instance.waitFor('ImmediateWait').timeout(1000).submit();

  assert.equal(new TextDecoder().decode(message.payload.payload), 'waited');
});

test('stream connector connect waits for an in-progress disconnect before reconnecting', async () => {
  let releaseClose;
  let closeStarted = false;
  const oldConnection = new MemoryConnection();
  oldConnection.read = async () => { throw new Error('old transport failed'); };
  oldConnection.close = async () => {
    closeStarted = true;
    await new Promise((resolve) => { releaseClose = resolve; });
    oldConnection.closed = true;
  };
  const newConnection = new MemoryConnection();
  let connectCalls = 0;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return ++connectCalls === 1 ? oldConnection : newConnection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    heartbeat: { enabled: false }
  });
  await instance.connect();
  // The receive loop owns the read in both dispatch modes, so its failure is
  // what starts the disconnect this scenario reconnects across.
  await waitFor(() => closeStarted, 1000);
  const reconnect = instance.connect();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(connectCalls, 1);
  releaseClose();
  await reconnect;
  assert.equal(connectCalls, 2);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Connected);
});

test('late read failure from a replaced connection does not disconnect a replacement', async () => {
  const reads = [];
  const oldConnection = new MemoryConnection();
  oldConnection.read = () => new Promise((_resolve, reject) => reads.push(reject));
  oldConnection.write = async () => { throw new Error('old heartbeat write failed'); };
  const newConnection = new MemoryConnection();
  let connectCalls = 0;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return ++connectCalls === 1 ? oldConnection : newConnection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    heartbeat: { intervalMs: 1, timeoutMs: 60000 }
  });
  await instance.connect();
  await waitFor(() => reads.length === 1, 1000);
  // The heartbeat send fails on the old transport only, so the connector
  // reconnects while the old connection still holds an unfinished read.
  await waitFor(
    () => connectCalls === 2 && instance.state === connector.ZlinkStreamConnectionState.Connected,
    2000
  );

  reads[0](new Error('late old read failed'));
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Connected);
  assert.equal(newConnection.closed, false);
});

test('a frame that arrives late on a replaced connection is discarded after reconnect', async () => {
  const reads = [];
  let connectCalls = 0;
  const connection = new MemoryConnection();
  connection.read = () => new Promise((resolve, reject) => reads.push({ resolve, reject }));
  // Only the first session fails its heartbeat send, so the transport object is
  // reused across the reconnect and the connection generation is the only thing
  // that tells the stale frame apart from a current one.
  connection.write = async () => {
    if (connectCalls === 1) throw new Error('old heartbeat write failed');
  };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { connectCalls++; return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    heartbeat: { intervalMs: 1, timeoutMs: 60000 }
  });
  const received = [];
  instance.on('OldMessage', (message) => received.push(message));

  await instance.connect();
  await waitFor(() => reads.length === 1, 1000);
  await waitFor(
    () => connectCalls === 2 && instance.state === connector.ZlinkStreamConnectionState.Connected,
    2000
  );

  reads[0].resolve(sendFrame('OldMessage', 'stale'));
  await new Promise((resolve) => setImmediate(resolve));
  await instance.dispatch();

  assert.deepEqual(received, []);
  assert.equal(connectCalls, 2);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Connected);
});

test('stream connector immediate receive failure closes transport and fails pending request', async () => {
  let rejectRead;
  const connection = new MemoryConnection();
  connection.read = () => new Promise((_resolve, reject) => {
    rejectRead = reject;
  });
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  const errors = [];
  instance.onErrorReceived((error) => errors.push(error.code));

  await instance.connect();
  const pending = instance.request({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: new Uint8Array()
  }).packetName('Pending').timeout(1000).submitEncoded();
  rejectRead(new Error('injected read failure'));

  await assert.rejects(() => pending, /Receive loop failed/);
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  assert.equal(connection.closed, true);
  assert.deepEqual(errors, [connector.ZlinkStreamErrorCode.FrameDecodeFailed]);
});

test('stream connector heartbeat timeout closes transport and fails pending request', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { intervalMs: 1, timeoutMs: 3 }
  });

  await instance.connect();
  const pending = instance.request({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: new Uint8Array()
  }).packetName('Pending').timeout(1000).submitEncoded();

  await assert.rejects(() => pending, /Heartbeat timed out/);
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  assert.equal(transportFactory.connection.closed, true);
});

test('stream connector heartbeat send failure closes transport and fails pending request', async () => {
  const connection = new MemoryConnection();
  const originalWrite = connection.write.bind(connection);
  connection.write = async (frame) => {
    const decoded = protocolCodecs.ZlinkStreamFrameCodec.decode(frame);
    const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(decoded.header);
    if (header.kind === connector.ZlinkStreamMessageKind.Control) {
      throw new Error('injected heartbeat write failure');
    }
    await originalWrite(frame);
  };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { intervalMs: 1, timeoutMs: 1000 }
  });
  const disconnected = [];
  instance.onDisconnected(() => disconnected.push(true));

  await instance.connect();
  const pending = instance.request({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: new Uint8Array()
  }).packetName('Pending').timeout(1000).submitEncoded();

  await assert.rejects(() => pending, /Heartbeat send failed/);
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  await instance.dispatch();
  assert.equal(connection.closed, true);
  assert.equal(disconnected.length, 1);
});

// D2 pin (spec 27 §2): a reply-less one-way Send never carries a correlation
// id. Only requests generate one; this pins the connector's compliant wire.
test('stream connector one-way send carries no correlation id on the wire', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });

  await instance.connect();
  await instance
    .send({ codec: connector.ZlinkStreamCodec.Raw, payload: new TextEncoder().encode('one-way') })
    .packetName('OneWay')
    .submit();

  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(header.correlationId, undefined);
  assert.equal(header.flags & connector.ZlinkStreamHeaderFlags.HasCorrelationId, 0);

  const pending = instance
    .request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
    .packetName('Corr')
    .timeout(1000)
    .submitEncoded();
  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[1]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  assert.notEqual(requestHeader.flags & connector.ZlinkStreamHeaderFlags.HasCorrelationId, 0);
  assert.ok(typeof requestHeader.correlationId === 'string' && requestHeader.correlationId.length > 0);
  transportFactory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Response,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq: requestHeader.requestSeq,
      name: 'Corr',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new Uint8Array()
  ));
  await instance.dispatch();
  await pending;
});

test('connector omits diagnostics surfaces and never sends flow fields', async () => {
  const factory = new MemoryTransportFactory();
  const instance = createStreamConnector({ endpoint: 'ws://127.0.0.1:19000', transportFactory: factory });
  assert.equal('diagnosticsLevel' in instance.options, false);
  assert.equal('diagnosticsLevel' in instance, false);
  assert.equal('setDiagnosticsLevel' in instance, false);
  assert.equal('setDiagnosticsLevelAsync' in instance, false);
  assert.equal('ZlinkStreamDiagnosticsLevel' in connector, false);
  await instance.connect();
  await instance.send({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }).packetName('NoFlow').submit();
  const send = protocolCodecs.ZlinkStreamHeaderCodec.decode(protocolCodecs.ZlinkStreamFrameCodec.decode(factory.connection.frames[0]).header);
  assert.equal(send.flags & connector.ZlinkStreamHeaderFlags.HasFlowId, 0);
  assert.equal(send.flags & connector.ZlinkStreamHeaderFlags.HasCorrelationId, 0);
  const pending = instance.request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }).packetName('NoFlowRequest').timeout(1000).submitEncoded();
  const request = protocolCodecs.ZlinkStreamHeaderCodec.decode(protocolCodecs.ZlinkStreamFrameCodec.decode(factory.connection.frames[1]).header);
  assert.equal(request.flags & connector.ZlinkStreamHeaderFlags.HasFlowId, 0);
  assert.notEqual(request.flags & connector.ZlinkStreamHeaderFlags.HasCorrelationId, 0);
  factory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Response, codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq, requestSeq: request.requestSeq,
    name: '', metadata: connector.ZlinkStreamMetadataMap.empty
  }), new Uint8Array()));
  await instance.dispatch();
  await pending;
});

test('inbound flow values are dropped while malformed lengths still fail', async () => {
  const wire = require('../../packages/stream-wire/dist');
  const header = wire.encodeStreamWireHeader({
    kind: connector.ZlinkStreamMessageKind.Send, codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.None, name: 'FlowInput', metadata: new Map(),
    flowId: '01890000-0000-7000-8000-000000000001', flowOrigin: 1
  });
  const invalidValue = header.slice();
  invalidValue[invalidValue.length - 5] = 122;
  assert.equal(protocolCodecs.ZlinkStreamHeaderCodec.decode(invalidValue).flowId, undefined);
  assert.throws(() => protocolCodecs.ZlinkStreamHeaderCodec.decode(header.slice(0, -1)), /incomplete/);
  const factory = new MemoryTransportFactory();
  const instance = createStreamConnector({ endpoint: 'ws://127.0.0.1:19000', transportFactory: factory });
  await instance.connect();
  const received = instance.waitForMessage('FlowInput', 1000, () => true);
  factory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(invalidValue, new Uint8Array([1])));
  await instance.dispatch();
  assert.equal('flowId' in await received, false);
});

test('connector on accepts an explicit name and a payload constructor', async () => {
  class TypedPush { static packetName = 'TypedPushWire'; }
  const factory = new MemoryTransportFactory();
  const instance = createStreamConnector({ endpoint: 'ws://127.0.0.1:19000', transportFactory: factory });
  const received = [];
  instance.on('TypedPushWire', (message) => received.push(message.name));
  instance.on(TypedPush, (message) => received.push(message.payload.value));
  await instance.connect();
  factory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Send, codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None, name: 'TypedPushWire',
    metadata: connector.ZlinkStreamMetadataMap.empty
  }), new TextEncoder().encode('{"value":7}')));
  await instance.dispatch();
  assert.deepEqual(received, ['TypedPushWire', 7]);
});

test('request hooks add metadata in order and report the full reply without changing its result', async () => {
  const factory = new MemoryTransportFactory();
  const instance = createStreamConnector({ endpoint: 'ws://127.0.0.1:19000', transportFactory: factory });
  const calls = [];
  instance.onRequestSending((ctx) => { calls.push('first'); ctx.setMetadata('first', '1'); });
  instance.onRequestSending((ctx) => { calls.push('second'); ctx.setMetadata('second', '2'); });
  instance.onReplyReceived((ctx) => { calls.push('reply'); assert.equal(ctx.succeeded, true); assert.equal(ctx.requestPacketName, 'Hooked'); assert.equal(ctx.reply.metadata.get('response'), 'ok'); assert.ok(ctx.elapsed >= 0); });
  await instance.connect();
  const pending = instance.request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array([3]) }).packetName('Hooked').timeout(1000).submitEncoded();
  assert.deepEqual(calls, ['first', 'second']);
  assert.equal(factory.connection.frames.length, 1);
  const sent = protocolCodecs.ZlinkStreamHeaderCodec.decode(protocolCodecs.ZlinkStreamFrameCodec.decode(factory.connection.frames[0]).header);
  assert.equal(sent.metadata.get('first'), '1');
  assert.equal(sent.metadata.get('second'), '2');
  factory.connection.pushFrame(protocolCodecs.ZlinkStreamFrameCodec.encode(protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Response, codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq, requestSeq: sent.requestSeq,
    name: '', metadata: connector.ZlinkStreamMetadataMap.empty.with('response', 'ok')
  }), new Uint8Array([4])));
  await instance.dispatch();
  assert.deepEqual(Array.from((await pending).payload), [4]);
  await instance.dispatch();
  assert.deepEqual(calls, ['first', 'second', 'reply']);
});

test('reply hook sees timeout and hook failures do not replace the request error', async () => {
  const factory = new MemoryTransportFactory();
  const instance = createStreamConnector({ endpoint: 'ws://127.0.0.1:19000', transportFactory: factory });
  const observed = [];
  const callbackErrors = [];
  instance.onRequestSending(() => { throw new Error('sending hook'); });
  instance.onErrorReceived((error) => callbackErrors.push({ code: error.code, sent: factory.connection.frames.length }));
  instance.onReplyReceived((ctx) => { observed.push(ctx); throw new Error('reply hook'); });
  await instance.connect();
  const pending = instance.request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }).packetName('Timeout').timeout(10).submitEncoded();
  assert.equal(factory.connection.frames.length, 1);
  assert.deepEqual(callbackErrors, []);
  await Promise.resolve();
  assert.deepEqual(callbackErrors[0], { code: connector.ZlinkStreamErrorCode.UserCallbackFailed, sent: 1 });
  await assert.rejects(pending, /timed out/);
  await instance.dispatch();
  assert.equal(observed.length, 1);
  assert.equal(observed[0].succeeded, false);
  assert.equal(observed[0].error.code, connector.ZlinkStreamErrorCode.RequestTimeout);
});

test('Manual sending hook runs inline without a dispatch pump', async () => {
  const factory = new MemoryTransportFactory();
  const instance = createStreamConnector({ endpoint: 'ws://127.0.0.1:19000', transportFactory: factory });
  const calls = [];
  instance.onRequestSending((ctx) => { calls.push('sending'); ctx.setMetadata('inline', 'yes'); });
  instance.onReplyReceived((ctx) => calls.push(ctx.error.code));
  await instance.connect();
  const pending = instance.request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }).packetName('Unsent').timeout(10).submitEncoded();
  assert.deepEqual(calls, ['sending']);
  assert.equal(factory.connection.frames.length, 1);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(protocolCodecs.ZlinkStreamFrameCodec.decode(factory.connection.frames[0]).header);
  assert.equal(header.metadata.get('inline'), 'yes');
  await assert.rejects(pending, /timed out/);
  await instance.dispatch();
  assert.deepEqual(calls, ['sending', connector.ZlinkStreamErrorCode.RequestTimeout]);
});

test('an already aborted request skips sending hook and reports the failed outcome', async () => {
  const factory = new MemoryTransportFactory();
  const instance = createStreamConnector({ endpoint: 'ws://127.0.0.1:19000', transportFactory: factory });
  const calls = [];
  instance.onRequestSending(() => calls.push('sending'));
  instance.onReplyReceived((ctx) => calls.push(ctx.error.code));
  await instance.connect();
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(instance.request({ codec: connector.ZlinkStreamCodec.Raw, payload: new Uint8Array() }).packetName('Canceled').submitEncoded(controller.signal), /canceled/);
  await instance.dispatch();
  assert.deepEqual(calls, [connector.ZlinkStreamErrorCode.Disconnected]);
  assert.equal(factory.connection.frames.length, 0);
});

class MemoryTransportFactory {
  constructor() {
    this.connection = new MemoryConnection();
  }

  async connect() {
    return this.connection;
  }
}

function withTimeout(promise, timeoutMs, label) {
  let timeout;
  return Promise.race([
    promise.finally(() => clearTimeout(timeout)),
    new Promise((_, reject) => {
      timeout = setTimeout(() => reject(new Error(`${label} timed out`)), timeoutMs);
    })
  ]);
}

async function waitFor(predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() >= deadline) {
      throw new Error('condition timed out');
    }
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
}

// Spec stream-connector 32 section 10: receivedCount(name) counts what arrived.
test('stream connector counts received packets per name without lowering them on consumption', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    heartbeat: { enabled: false }
  });
  await instance.connect();
  assert.equal(instance.receivedCount('Notice'), 0);

  transportFactory.connection.pushFrame(sendFrame('Notice', 'first'));
  await instance.dispatch();
  assert.equal(instance.receivedCount('Notice'), 1);

  // A wait surface consumes the packet, and the count stays where it was.
  const observed = await instance.waitFor('Notice').timeout(1000).submit();
  assert.equal(observed.name, 'Notice');
  assert.equal(instance.receivedCount('Notice'), 1);

  // A registered handler in Manual mode changes nothing either: the packet is
  // counted where it arrives, not where the pump hands it over.
  const delivered = [];
  instance.on('Notice', (message) => { delivered.push(message); });
  transportFactory.connection.pushFrame(sendFrame('Notice', 'second'));
  await instance.dispatch();
  assert.equal(delivered.length, 1);
  assert.equal(instance.receivedCount('Notice'), 2);
  assert.equal(instance.receivedCount('Unseen'), 0);
  await instance.close();
});

test('stream connector counts received packets the same way in immediate dispatch', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false }
  });
  const delivered = [];
  instance.on('Notice', (message) => { delivered.push(message); });
  await instance.connect();

  transportFactory.connection.pushFrame(sendFrame('Notice', 'first'));
  for (let attempt = 0; attempt < 500 && delivered.length === 0; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
  assert.equal(delivered.length, 1);
  assert.equal(instance.receivedCount('Notice'), 1);
  await instance.close();
});

test('stream connector restarts received counts when a connection is established', async () => {
  const transportFactory = new ReconnectingTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  await instance.connect();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'first'));
  await instance.dispatch();
  assert.equal(instance.receivedCount('Notice'), 1);
  await instance.close();

  const reconnected = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  await reconnected.connect();
  assert.equal(reconnected.receivedCount('Notice'), 0);
  await reconnected.close();
});

// Spec stream-connector 32 section 10.1: both ways of naming a packet exist.
test('stream wait surfaces accept a payload constructor as well as a name', async () => {
  class Notice {
    static get packetName() { return 'Notice'; }
  }
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    heartbeat: { enabled: false }
  });
  await instance.connect();

  const waited = instance.waitFor(Notice).timeout(1000).submit();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'typed'));
  await instance.dispatch();
  assert.equal((await waited).name, 'Notice');

  await instance.expectNone(Notice).within(5).run();

  const sequence = instance.waitForSequence(Notice).expect(() => true).timeout(1000).run();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'typed again'));
  await instance.dispatch();
  assert.deepEqual((await sequence).map((message) => message.name), ['Notice']);
  await instance.close();
});

// Spec stream-connector 32 section 5: a static member names the type, and the
// constructor name is only the fallback.
test('default packet name resolver prefers the static packetName member', () => {
  class WithStatic {
    static get packetName() { return 'inventory.changed'; }
  }
  class WithoutStatic {}
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: new MemoryTransportFactory()
  });

  assert.equal(instance.options.nameResolver.resolve(WithStatic), 'inventory.changed');
  assert.equal(instance.options.nameResolver.resolve(WithoutStatic), 'WithoutStatic');
});

// Spec stream-connector 32 section 6: null means unlimited.
test('stream connector accepts null reconnect attempts as unlimited and rejects bad numbers', () => {
  const unlimited = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: new MemoryTransportFactory(),
    reconnect: { maxAttempts: null }
  });
  assert.equal(unlimited.options.reconnect.maxAttempts, null);

  for (const maxAttempts of [0, -1]) {
    assert.throws(
      () => connector.zlinkStreamConnectorFactory.create({
        endpoint: 'ws://127.0.0.1:19000',
        transportFactory: new MemoryTransportFactory(),
        reconnect: { maxAttempts }
      }),
      (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
    );
  }
});

test('stream connector keeps retrying while reconnect attempts are unlimited', async () => {
  const transportFactory = new FlakyTransportFactory(4);
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: null },
    heartbeat: { enabled: false }
  });

  await instance.connect();

  assert.equal(transportFactory.attempts, 5);
  assert.equal(instance.isConnected, true);
  await instance.close();
});

// Spec stream-connector 32 section 6: the wait is a value in [50%, 100%] of the
// base delay, so clients that dropped together do not return together.
test('stream connector randomizes the wait between reconnect attempts', async () => {
  const baseDelayMs = 200;
  const factors = [0, 0.5, 0.9999];
  const flaky = new FlakyTransportFactory(3);
  const originalRandom = Math.random;
  let index = 0;
  Math.random = () => factors[Math.min(index++, factors.length - 1)];
  const attemptAt = [];
  const timedFactory = {
    async connect(options, signal) {
      attemptAt.push(Date.now());
      return await flaky.connect(options, signal);
    }
  };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: timedFactory,
    reconnect: { initialDelayMs: baseDelayMs, maxDelayMs: baseDelayMs, backoffFactor: 1, maxAttempts: 4 },
    heartbeat: { enabled: false }
  });
  try {
    await instance.connect();
  } finally {
    Math.random = originalRandom;
  }

  assert.equal(attemptAt.length, 4);
  const waits = attemptAt.slice(1).map((value, position) => value - attemptAt[position]);
  const expected = factors.map((factor) => Math.round(baseDelayMs * (0.5 + factor * 0.5)));
  for (const [position, wait] of waits.entries()) {
    // A timer never fires early, and a busy event loop may fire it late.
    assert.ok(
      wait >= expected[position] - 20 && wait <= expected[position] + 250,
      `wait ${wait}ms is not close to the expected ${expected[position]}ms`
    );
    assert.ok(
      wait >= baseDelayMs * 0.5 - 20 && wait <= baseDelayMs + 250,
      `wait ${wait}ms left the 50%-100% window of ${baseDelayMs}ms`
    );
  }
  await instance.close();
});

test('stream connector runs the disconnect handler when reconnect attempts are spent', async () => {
  const transportFactory = new FlakyTransportFactory(5);
  let disconnected = 0;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    reconnect: { initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });
  instance.onDisconnected(() => { disconnected += 1; });

  await assert.rejects(() => instance.connect(), /Connect failed/);

  assert.equal(transportFactory.attempts, 2);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Disconnected);
  assert.equal(disconnected, 1);
  assert.equal(instance.closeReason, 'TransportError');
});

test('zlinkStreamAssert.ensure refuses an empty diagnostic message', () => {
  assert.throws(
    () => connector.zlinkStreamAssert.ensure(true, '   '),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
      && /non-empty diagnostic message/.test(error.error.message)
  );
  assert.throws(
    () => connector.zlinkStreamAssert.ensure(false, ''),
    (error) => /non-empty diagnostic message/.test(error.error.message)
  );
  connector.zlinkStreamAssert.ensure(true, 'still fine');
});

class ReconnectingTransportFactory {
  constructor() {
    this.connection = new MemoryConnection();
  }

  async connect() {
    return this.connection;
  }
}

function sendFrame(name, payload) {
  return protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name,
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode(payload)
  );
}

function sendFrameForActor(name, payload, actorSlot) {
  return protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.HasActorSlot,
      name,
      metadata: connector.ZlinkStreamMetadataMap.empty,
      actorSlot
    }),
    new TextEncoder().encode(payload)
  );
}

function actorControlFrame(name, payload) {
  return protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Control,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name,
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    Uint8Array.from(payload)
  );
}

function unpickleLz4(payload) {
  if (payload.length === 0) {
    return new Uint8Array();
  }
  assert.equal(payload[0], 0);
  return payload.slice(1);
}

class MemoryConnection {
  constructor() {
    this.frames = [];
    this.inbound = [];
    this.closed = false;
    this.writeWaiters = [];
  }

  async write(frame) {
    this.frames.push(frame);
    const waiter = this.writeWaiters.shift();
    if (waiter !== undefined) {
      waiter(frame);
    }
  }

  async read() {
    return this.inbound.shift();
  }

  async nextWrite() {
    const frame = this.frames.shift();
    if (frame !== undefined) {
      return frame;
    }
    return await new Promise((resolve) => this.writeWaiters.push(resolve));
  }

  pushFrame(frame) {
    this.inbound.push(frame);
  }

  async close() {
    this.closed = true;
  }
}

class FlakyTransportFactory {
  constructor(failures) {
    this.failures = failures;
    this.attempts = 0;
    this.connection = new MemoryConnection();
  }

  async connect() {
    this.attempts += 1;
    if (this.attempts <= this.failures) {
      throw new Error('connect failed');
    }
    return this.connection;
  }
}

// Issue #583 (1): `dispatch` inside a registered handler is re-entry, not a
// second pump. The handler runs from inside the drain the pump is awaiting, so
// a pump that waited for the drain task again would be waiting for itself.
test('a dispatch made from inside a message handler returns instead of waiting on its own drain', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  const order = [];
  instance.on('Nested', async () => {
    order.push('nested-enter');
    await instance.dispatch();
    order.push('nested-exit');
  });
  instance.on('Follow', () => { order.push('follow'); });

  await instance.connect();
  transportFactory.connection.pushFrame(sendFrame('Nested', 'first'));
  transportFactory.connection.pushFrame(sendFrame('Follow', 'second'));

  await withTimeout(instance.dispatch(), 2000, 'dispatch from inside a handler');

  // The nested call is a no-op, and the drain it re-entered still delivers the
  // rest of the queue afterwards.
  assert.deepEqual(order, ['nested-enter', 'nested-exit', 'follow']);
});

// Issue #583 (2): `connect` waits for the disconnect task, so the disconnect
// handler must run outside it. A handler that reconnects would otherwise wait
// for the task its own caller has not yet left.
test('a disconnect handler may call connect without waiting on the disconnect that ran it', async () => {
  const oldConnection = new MemoryConnection();
  oldConnection.read = async () => { throw new Error('old transport failed'); };
  const newConnection = new MemoryConnection();
  let connectCalls = 0;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return ++connectCalls === 1 ? oldConnection : newConnection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  let handlerFailure;
  instance.onDisconnected(async () => {
    await withTimeout(instance.connect(), 2000, 'connect from the disconnect handler')
      .catch((error) => { handlerFailure = error; });
  });

  await instance.connect();
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
  await instance.dispatch();
  await waitFor(
    () => connectCalls === 2 && instance.state === connector.ZlinkStreamConnectionState.Connected,
    3000
  );
  assert.equal(handlerFailure, undefined);
});

// Issue #583 (2): `publishDisconnected` settles every handler, so a handler
// promise that never settles never lets it return. The reconnect the spec has
// on by default must not sit behind that.
test('a disconnect handler that never settles does not cost the connector its reconnect', async () => {
  const oldConnection = new MemoryConnection();
  oldConnection.read = async () => { throw new Error('old transport failed'); };
  const newConnection = new MemoryConnection();
  let connectCalls = 0;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return ++connectCalls === 1 ? oldConnection : newConnection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });
  let handlerEntered = false;
  let releaseHandler;
  // Unsettled for the whole reconnect window. It is released at the end only so
  // that the shared cleanup, which notifies the same handler once more, can
  // finish; nothing under test waits for it.
  const unsettled = new Promise((resolve) => { releaseHandler = resolve; });
  instance.onDisconnected(() => {
    handlerEntered = true;
    return unsettled;
  });

  await instance.connect();
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Connected && connectCalls === 2, 3000);
  await instance.dispatch();
  await waitFor(
    () => connectCalls === 2 && instance.state === connector.ZlinkStreamConnectionState.Connected,
    3000
  );
  assert.equal(handlerEntered, true);
  releaseHandler();
});

// Issue #583 (3): the batch loop awaits application code between frames, and a
// disconnect can complete inside that await. Every frame after it belongs to a
// connection the connector no longer holds.
test('frames left in a batch of a connection that was torn down mid-batch are dropped', async () => {
  const closingFrame = protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Control,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: 'session-closing',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    Uint8Array.from([1, 4, 0, 0])
  );
  const late = sendFrame('LateInBatch', 'stale');
  const batch = new Uint8Array(closingFrame.length + late.length);
  batch.set(closingFrame, 0);
  batch.set(late, closingFrame.length);

  let delivered = false;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false },
    transportFactory: {
      async connect() {
        return {
          async write() {},
          async read() {
            if (delivered) return undefined;
            delivered = true;
            return batch;
          },
          async close() {}
        };
      }
    }
  });
  const received = [];
  instance.on('LateInBatch', (message) => received.push(message));

  await instance.connect();
  await instance.dispatch();
  assert.equal(instance.closeReason, 'ServerDrain');
  // The receive loop finishes the batch after that pump has returned, so the
  // verdict is only meaningful once the loop has had its turn and a second
  // pump has had the chance to deliver whatever the batch left queued.
  await new Promise((resolve) => setTimeout(resolve, 50));
  await instance.dispatch();

  assert.deepEqual(received, []);
  // Spec stream-connector 32 section 10 counts arrivals on the current
  // connection, and this frame arrived on one that no longer exists.
  assert.equal(instance.receivedCount('LateInBatch'), 0);
});

// Issue #583 (4): the interval fires on the clock, not on the previous tick.
test('an unfinished heartbeat tick suppresses the tick the interval asks for next', async () => {
  const writes = [];
  const releases = [];
  const connection = new MemoryConnection();
  connection.write = (frame) => {
    writes.push(frame);
    return new Promise((resolve) => { releases.push(resolve); });
  };
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { intervalMs: 1, timeoutMs: 60000 }
  });

  await instance.connect();
  await waitFor(() => writes.length === 1, 1000);
  // Dozens of interval periods pass while the first ping is still unwritten.
  await new Promise((resolve) => setTimeout(resolve, 60));
  assert.equal(writes.length, 1);

  for (const release of releases.splice(0)) {
    release();
  }
  await instance.close();
});

// Spec stream-connector 32 §7: Manual submits the disconnect handler to the
// dispatch queue, and dispatch starts it without waiting for completion. A
// handler that calls `close` must therefore not wait on its own invocation.
test('a disconnect handler may call close without waiting on the close that ran it', async () => {
  // Deliberately outside the shared cleanup: `close` is the subject here, and
  // the cleanup's own close would re-enter it. Heartbeat is off, so this
  // connector holds no timer once the scenario is done with it.
  const instance = connector.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: new MemoryTransportFactory(),
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  let handlerEntered = false;
  let nestedSettled = false;
  let nestedFailure;
  instance.onDisconnected(async () => {
    handlerEntered = true;
    // Settled through callbacks rather than a bare await, so that a build
    // without the fix leaves a pending promise nobody reports as unhandled.
    await instance.close().then(
      () => { nestedSettled = true; },
      (error) => { nestedSettled = true; nestedFailure = error; }
    );
  });

  await instance.connect();
  await withTimeout(instance.close(), 2000, 'close called from inside a disconnect handler');
  await withTimeout(instance.dispatch(), 2000, 'dispatch called from inside a disconnect handler');

  // Dispatch ran the handler but did not wait for its completion.
  assert.equal(handlerEntered, true);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Closed);
  await waitFor(() => nestedSettled, 2000);
  assert.equal(nestedFailure, undefined);
});

// Issue #583 (5): spec stream-connector 32 section 10 (line ~649) says a
// connection that is established drops whatever the previous connection left
// unconsumed, not only its counts — otherwise the counts and the queue
// describe two different connections. Before the fix, the lifecycle reset
// only `receivedCounts` on reconnect and left the queue in place, so a
// `waitFor` registered after the reconnect could still be handed a message
// the dead connection delivered.
test('reconnecting drops the previous connection\'s unconsumed queue, not only its counts', async () => {
  let connectCalls = 0;
  let oldReads = 0;
  const oldConnection = new MemoryConnection();
  oldConnection.read = async () => {
    oldReads += 1;
    // The first read delivers a message nobody consumes; every read after it
    // fails, which is what drives the disconnect into a reconnect.
    if (oldReads === 1) return sendFrame('OldMessage', 'stale');
    throw new Error('old connection read failed');
  };
  const newConnection = new MemoryConnection();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: {
      async connect() {
        connectCalls += 1;
        return connectCalls === 1 ? oldConnection : newConnection;
      }
    },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: true, initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });

  await instance.connect();
  // No handler is registered for 'OldMessage', so it arrives, is counted, and
  // stays queued — nobody dispatches it.
  await waitFor(() => instance.receivedCount('OldMessage') === 1, 1000);

  await waitFor(
    () => connectCalls === 2 && instance.state === connector.ZlinkStreamConnectionState.Connected,
    2000
  );

  assert.equal(instance.receivedCount('OldMessage'), 0);
  // A `waitFor` registered fresh on the new connection must actually wait —
  // not be handed the stale message the dead connection queued — and time out
  // (spec 32 section 10.1.1: a wait that times out is ValidationFailed).
  await assert.rejects(
    instance.waitFor('OldMessage').timeout(50).submit(),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
});

// Issue #583 (5): a `waitFor` already registered when the connection it is
// watching ends is abandoned right there (spec stream-connector 32 section
// 10.1: "연결이 끝나 대기를 이어갈 수 없으면 Disconnected다"), matching Java
// `ZLinkStreamDispatchQueue.resetForNewConnection`'s
// `replacesAnEarlierConnection` waiter failure. It must not keep waiting
// through the reconnect only to time out as if nothing had arrived.
test('a waitFor pending across a reconnect rejects as disconnected instead of waiting on the new connection', async () => {
  let connectCalls = 0;
  const oldConnection = new MemoryConnection();
  oldConnection.read = () => new Promise((_resolve, reject) => {
    oldConnection.rejectRead = reject;
  });
  const newConnection = new MemoryConnection();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: {
      async connect() {
        connectCalls += 1;
        return connectCalls === 1 ? oldConnection : newConnection;
      }
    },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: true, initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });

  await instance.connect();
  const pending = instance.waitFor('NeverArrives').timeout(5000).submit();
  // Attached before the rejection fires, in the same tick `pending` is
  // created, so this is the promise's real handler rather than a second one
  // racing an "unhandledRejection" the reconnect below would otherwise raise.
  const pendingRejection = assert.rejects(
    pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.Disconnected
  );

  oldConnection.rejectRead(new Error('old connection read failed'));
  await waitFor(
    () => connectCalls === 2 && instance.state === connector.ZlinkStreamConnectionState.Connected,
    2000
  );

  await pendingRejection;
});

// Spec stream-connector 32 section 10.1.1: a wait is released with
// `Disconnected` when the connection it observed ends - not when the next
// connection is established. With reconnect off there is no next connection,
// so a release bound to it would leave the wait hanging until its own timeout.
test('a waitFor pending when the connection ends without a reconnect rejects as disconnected at once', async () => {
  const connection = new MemoryConnection();
  connection.read = () => new Promise((_resolve, reject) => {
    connection.rejectRead = reject;
  });
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return connection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });

  await instance.connect();
  const pending = instance.waitFor('NeverArrives').timeout(5000).submit();
  const startedAt = Date.now();
  const pendingRejection = assert.rejects(
    pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.Disconnected
  );

  connection.rejectRead(new Error('connection read failed'));
  await pendingRejection;
  // Released by the ending, not by the 5 s wait timeout.
  assert.ok(Date.now() - startedAt < 1000, 'the wait waited for its own timeout');
  await waitFor(() => instance.state === connector.ZlinkStreamConnectionState.Disconnected, 1000);
});

// Spec stream-connector 32 section 10.1.1: with reconnect on, the wait still
// ends at the ending of its connection, before the reconnect produces the next
// one. The second connect is held open until the wait has been observed.
test('a waitFor pending when the connection ends rejects as disconnected before the reconnect succeeds', async () => {
  let connectCalls = 0;
  let secondConnectionProduced = false;
  let releaseSecondConnect;
  const secondConnectReleased = new Promise((resolve) => { releaseSecondConnect = resolve; });
  const oldConnection = new MemoryConnection();
  oldConnection.read = () => new Promise((_resolve, reject) => {
    oldConnection.rejectRead = reject;
  });
  const newConnection = new MemoryConnection();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: {
      async connect() {
        connectCalls += 1;
        if (connectCalls === 1) return oldConnection;
        await secondConnectReleased;
        secondConnectionProduced = true;
        return newConnection;
      }
    },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    reconnect: { enabled: true, initialDelayMs: 1, maxDelayMs: 1, backoffFactor: 1, maxAttempts: 2 },
    heartbeat: { enabled: false }
  });

  await instance.connect();
  const pending = instance.waitFor('NeverArrives').timeout(5000).submit();
  const startedAt = Date.now();
  const pendingRejection = assert.rejects(
    pending,
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.Disconnected
  );

  oldConnection.rejectRead(new Error('old connection read failed'));
  try {
    await pendingRejection;
    assert.ok(Date.now() - startedAt < 1000, 'the wait outlived the ending of its connection');
    // No second connection exists yet: the release came from the ending.
    assert.equal(secondConnectionProduced, false);
  } finally {
    // Released whatever the assertions said, so a failure does not leave the
    // suite's closing hook waiting on a connect that never returns.
    releaseSecondConnect();
  }
  await waitFor(
    () => connectCalls === 2 && instance.state === connector.ZlinkStreamConnectionState.Connected,
    2000
  );
});

// Spec stream-connector 32 section 10.1.1: nothing arriving inside the window
// is a violated observation, `ValidationFailed`. `RequestTimeout` is the code
// of a request whose reply did not come (section 9), not of this surface.
test('waitFor and waitForSequence time out as validationFailed while expectNone passes on the same silence', async () => {
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: new MemoryTransportFactory(),
    reconnect: { enabled: false },
    heartbeat: { enabled: false }
  });
  await instance.connect();

  await assert.rejects(
    instance.waitFor('Silent').timeout(20).submit(),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
  await assert.rejects(
    instance.waitForSequence('Silent').expect(() => true).timeout(20).run(),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );
  await instance.expectNone('Silent').within(20).run();
  await instance.close();
});
