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

  await assert.rejects(() => sendInstance.send({
    codec: connector.ZlinkStreamCodec.Raw,
    payload: new TextEncoder().encode('bb')
  }).packetName('h').submit());
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
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.FrameTooLarge
  );
  assert.equal(requestTransportFactory.connection.frames.length, 0);
  assert.equal(requestInstance.pendingDispatchCount, 0);
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
    maxReceivePayloadSize: 1
  });
  const errors = [];
  let received = false;
  instance.onErrorReceived((error) => { errors.push(error.code); });
  instance.on('TooLarge', () => { received = true; });
  await instance.connect();
  transportFactory.connection.pushFrame(sendFrame('TooLarge', 'bb'));

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

test('stream connector inbound observer sees response before pending request completes without waiting for callback', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  let releaseObserver;
  const observerRelease = new Promise((resolve) => {
    releaseObserver = resolve;
  });
  let resolveObserved;
  const observed = new Promise((resolve) => {
    resolveObserved = resolve;
  });
  const registration = instance.observeInbound(async (observation) => {
    resolveObserved(observation);
    await observerRelease;
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
  const snapshot = await withTimeout(observed, 1000, 'inbound response observer');
  const reply = await pending;

  assert.equal(snapshot.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(snapshot.name, '');
  assert.equal(snapshot.requestSeq, requestHeader.requestSeq);
  assert.equal(snapshot.payloadLength, '{"accepted":true}'.length);
  assert.equal(new TextDecoder().decode(reply.payload), '{"accepted":true}');
  releaseObserver();
  registration.dispose();
});

test('stream connector inbound observer sees send and control frames', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const observations = [];
  const registration = instance.observeInbound((observation) => {
    observations.push(observation);
  });
  const received = [];
  instance.on('Notice', (message) => {
    received.push(new TextDecoder().decode(message.payload.payload));
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('body')
    )
  );
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Control,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.None,
        name: '$zlink.heartbeat.pong',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new Uint8Array()
    )
  );

  await instance.dispatch();
  await instance.dispatch();
  await waitFor(() => observations.some((item) => item.kind === connector.ZlinkStreamMessageKind.Control), 1000);

  assert.deepEqual(received, ['body']);
  assert.ok(observations.some((item) => item.kind === connector.ZlinkStreamMessageKind.Send && item.name === 'Notice'));
  assert.ok(observations.some((item) => item.kind === connector.ZlinkStreamMessageKind.Control));
  registration.dispose();
});

test('stream connector inbound observer metadata snapshot cannot change handler metadata', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const registration = instance.observeInbound((observation) => {
    observation.metadata.values.set('trace', 'mutated');
  });
  let receivedTrace;
  instance.on('Notice', (message) => {
    receivedTrace = message.metadata.get('trace');
  });

  await instance.connect();
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Send,
        codec: connector.ZlinkStreamCodec.Raw,
        flags: connector.ZlinkStreamHeaderFlags.HasMetadata,
        name: 'Notice',
        metadata: connector.ZlinkStreamMetadataMap.empty.with('trace', 'original')
      }),
      new TextEncoder().encode('body')
    )
  );

  await instance.dispatch();
  await waitFor(() => receivedTrace !== undefined, 1000);

  assert.equal(receivedTrace, 'original');
  registration.dispose();
});

test('stream connector inbound observer rejects after connect and stops after dispose', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  let observed = 0;
  const registration = instance.observeInbound(() => {
    observed += 1;
  });

  await instance.connect();
  assert.throws(
    () => instance.observeInbound(() => {}),
    (error) => error.error?.code === connector.ZlinkStreamErrorCode.ValidationFailed
  );

  transportFactory.connection.pushFrame(sendFrame('First', 'a'));
  await instance.dispatch();
  await waitFor(() => observed === 1, 1000);
  registration.dispose();
  transportFactory.connection.pushFrame(sendFrame('Second', 'b'));
  await instance.dispatch();
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(observed, 1);
});

test('stream connector inbound observer failure reports observer-failed and dispatch continues', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });
  instance.observeInbound(() => {
    throw new Error('observer failed');
  });
  const received = [];
  instance.on('Notice', () => {
    received.push('handled');
  });

  await instance.connect();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'body'));
  await instance.dispatch();
  await waitFor(() => errors.some((error) => error.code === connector.ZlinkStreamErrorCode.ObserverFailed), 1000);

  assert.deepEqual(received, ['handled']);
});

test('stream connector inbound observer overflow reports observer-dropped and request still completes', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    maxInboundObserverNotifications: 1
  });
  let releaseObserver;
  const observerRelease = new Promise((resolve) => {
    releaseObserver = resolve;
  });
  const observedNames = [];
  instance.observeInbound(async (observation) => {
    observedNames.push(observation.name);
    await observerRelease;
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });

  await instance.connect();
  for (let index = 0; index < 3; index += 1) {
    const pending = instance
      .request({
        codec: connector.ZlinkStreamCodec.Json,
        payload: new TextEncoder().encode(`{"request":${index}}`)
      })
      .packetName('Join')
      .timeout(1000)
      .submitEncoded();

    const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[index]);
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
        new TextEncoder().encode(`{"accepted":${index}}`)
      )
    );
    await instance.dispatch();
    const reply = await pending;
    assert.equal(new TextDecoder().decode(reply.payload), `{"accepted":${index}}`);
  }

  await waitFor(() => errors.some((error) => error.code === connector.ZlinkStreamErrorCode.ObserverDropped), 1000);
  releaseObserver();
  await waitFor(() => observedNames.length >= 2, 1000);
  assert.deepEqual(observedNames, ['', '']);
});

test('stream connector received-message queue overflow is separate from inbound observer queue', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    maxReceivedMessages: 1,
    maxInboundObserverNotifications: 10
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });
  let releaseHandler;
  const handlerRelease = new Promise((resolve) => {
    releaseHandler = resolve;
  });
  const received = [];
  instance.on('Notice', async (message) => {
    received.push(new TextDecoder().decode(message.payload.payload));
    await handlerRelease;
  });

  await instance.connect();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'first'));
  transportFactory.connection.pushFrame(sendFrame('Notice', 'second'));
  transportFactory.connection.pushFrame(sendFrame('Notice', 'third'));
  await instance.dispatch();
  await instance.dispatch();
  await instance.dispatch();
  await waitFor(() => errors.some((error) => error.code === connector.ZlinkStreamErrorCode.ReceivedMessageDropped), 1000);
  releaseHandler();
  await waitFor(() => received.length === 2, 1000);

  assert.deepEqual(received, ['first', 'second']);
  assert.equal(errors.some((error) => error.code === connector.ZlinkStreamErrorCode.ObserverDropped), false);
});

test('stream connector received-message cap does not block request response frames', async () => {
  const transportFactory = new MemoryTransportFactory();
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory,
    maxReceivedMessages: 1
  });
  const errors = [];
  instance.onErrorReceived((error) => {
    errors.push(error);
  });
  let releaseHandler;
  const handlerRelease = new Promise((resolve) => {
    releaseHandler = resolve;
  });
  const received = [];
  instance.on('Notice', async (message) => {
    received.push(new TextDecoder().decode(message.payload.payload));
    await handlerRelease;
  });

  await instance.connect();
  transportFactory.connection.pushFrame(sendFrame('Notice', 'running'));
  await instance.dispatch();
  await waitFor(() => received.length === 1, 1000);
  transportFactory.connection.pushFrame(sendFrame('Notice', 'queued'));
  await instance.dispatch();

  const pending = instance
    .request({
      codec: connector.ZlinkStreamCodec.Json,
      payload: new TextEncoder().encode('{"request":1}')
    })
    .packetName('Lookup')
    .timeout(1000)
    .submitEncoded();
  const requestFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(transportFactory.connection.frames[0]);
  const requestHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(requestFrame.header);
  transportFactory.connection.pushFrame(
    protocolCodecs.ZlinkStreamFrameCodec.encode(
      protocolCodecs.ZlinkStreamHeaderCodec.encode({
        kind: connector.ZlinkStreamMessageKind.Response,
        codec: connector.ZlinkStreamCodec.Json,
        flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
        requestSeq: requestHeader.requestSeq,
        name: 'Lookup',
        metadata: connector.ZlinkStreamMetadataMap.empty
      }),
      new TextEncoder().encode('{"ok":true}')
    )
  );
  await instance.dispatch();
  const reply = await pending;
  releaseHandler();
  await waitFor(() => received.length === 2, 1000);

  assert.equal(new TextDecoder().decode(reply.payload), '{"ok":true}');
  assert.deepEqual(received, ['running', 'queued']);
  assert.equal(errors.some((error) => error.code === connector.ZlinkStreamErrorCode.ReceivedMessageDropped), false);
});

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
  const payloads = await sequence;
  assert.deepEqual(payloads.map((payload) => new TextDecoder().decode(payload.payload)), ['first', 'second']);

  const outOfOrder = instance.waitForSequence('Notice')
    .expect((payload) => new TextDecoder().decode(payload.payload) === 'first')
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
    transportFactory
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

  assert.equal(transportFactory.connection.closed, true);
  assert.deepEqual(events.slice(-2), [
    `state:${connector.ZlinkStreamConnectionState.Closed}`,
    'disconnected'
  ]);
  assert.equal(events.filter((event) => event === 'disconnected').length, 1);
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

  await assert.rejects(() => instance.dispatch(), /pong write failed|Send failed/);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Disconnected);
  assert.equal(connection.closed, true);
  assert.deepEqual(errors, [connector.ZlinkStreamErrorCode.SendFailed]);
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
  const failingDispatch = instance.dispatch();
  await waitFor(() => closeStarted, 1000);
  const reconnect = instance.connect();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(connectCalls, 1);
  releaseClose();
  await assert.rejects(failingDispatch, /old transport failed|Stream dispatch failed/);
  await reconnect;
  assert.equal(connectCalls, 2);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Connected);
});

test('late dispatch failure from an old connection does not disconnect a replacement', async () => {
  const reads = [];
  const oldConnection = new MemoryConnection();
  oldConnection.read = () => new Promise((_resolve, reject) => reads.push(reject));
  const newConnection = new MemoryConnection();
  let connectCalls = 0;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { return ++connectCalls === 1 ? oldConnection : newConnection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    heartbeat: { enabled: false }
  });
  await instance.connect();
  const first = instance.dispatch();
  const second = instance.dispatch();
  await waitFor(() => reads.length === 2, 1000);
  reads[0](new Error('first old read failed'));
  await assert.rejects(first, /first old read failed|Stream dispatch failed/);
  await instance.connect();
  reads[1](new Error('late old read failed'));
  await assert.rejects(second, /late old read failed|Stream dispatch failed/);
  assert.equal(instance.state, connector.ZlinkStreamConnectionState.Connected);
  assert.equal(newConnection.closed, false);
});

test('late successful dispatch from an old connection is discarded after reconnect', async () => {
  const reads = [];
  const oldConnection = new MemoryConnection();
  oldConnection.read = () => new Promise((resolve, reject) => reads.push({ resolve, reject }));
  let connectCalls = 0;
  const instance = createStreamConnector({
    endpoint: 'ws://127.0.0.1:19000',
    transportFactory: { async connect() { connectCalls++; return oldConnection; } },
    dispatchMode: connector.ZlinkStreamDispatchMode.Manual,
    heartbeat: { enabled: false }
  });
  const received = [];
  instance.on('OldMessage', (message) => received.push(message));
  await instance.connect();
  const first = instance.dispatch();
  const stale = instance.dispatch();
  await waitFor(() => reads.length === 2, 1000);
  reads[0].reject(new Error('disconnect old connection'));
  await assert.rejects(first, /disconnect old connection|Stream dispatch failed/);
  await instance.connect();
  reads[1].resolve(protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Raw,
      flags: connector.ZlinkStreamHeaderFlags.None,
      name: 'OldMessage',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode('stale')
  ));
  await stale;
  await new Promise((resolve) => setImmediate(resolve));
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
  await waitFor(() => disconnected.length === 1, 1000);
  assert.equal(connection.closed, true);
  assert.equal(disconnected.length, 1);
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
