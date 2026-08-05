'use strict';

// MFLOW-xxx: message-flow tracing parity (mirrors C++/.NET/Java). Exercises the tracer's
// mode gating (zero-cost off), structured node-stamped output, file routing, observer
// offload, the live-mode toggle, and the stream correlation_id wire round-trip
// (framework <-> connector codecs byte-identical).

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const { ZLinkStreamFrameMessageFactory } = require('../../packages/framework/dist/runtime/streams/stream-frame-factory');
const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');

const {
  ZLinkMessageFlowTracer,
  createDiagnosticsContext,
  createMessageFlowModeCell,
  ZLinkMessageFlowLogMode
} = framework;

const ZLinkMessageFlowOutcome = {
  Received: 'received',
  Dispatched: 'dispatched',
  Replied: 'replied',
  Dropped: 'dropped'
};
const ZLinkDispatchErrorSurface = { Channel: 'channel' };
const ZLinkDispatchMessageKind = { Request: 'request' };

function silentSink() {
  return { reportRuntimeTaskException() {} };
}

function diagnostics(messageFlow, overrides = {}) {
  return {
    messageFlow,
    sampleRate: 1,
    includeMessageSizes: false,
    ...overrides
  };
}

function makeTracer(diagnosticsOptions, providerResolver, messageFlowObserverType) {
  const dispatch = { diagnostics: diagnosticsOptions, providerResolver };
  const cell = createMessageFlowModeCell(dispatch);
  const ctx = {
    ...createDiagnosticsContext(dispatch, providerResolver, cell),
    messageFlowObserverType
  };
  return { tracer: new ZLinkMessageFlowTracer(ctx, silentSink()), cell };
}

function receivedEvent() {
  return {
    outcome: ZLinkMessageFlowOutcome.Received,
    surface: ZLinkDispatchErrorSurface.Channel,
    messageKind: ZLinkDispatchMessageKind.Request,
    packetName: 'EchoRequest',
    channelName: 'api',
    correlationId: 'corr-1'
  };
}

test('MFLOW-001 off mode is zero-cost: enabled() false and trace() is a no-op', () => {
  const { tracer } = makeTracer(diagnostics(ZLinkMessageFlowLogMode.Off));
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), false);
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Dropped), false);
  tracer.trace(receivedEvent());
  assert.equal(tracer.tracedCount, 0);
});

test('MFLOW-002 mode ladder gates phases by severity', () => {
  const errorsOnly = makeTracer(diagnostics(ZLinkMessageFlowLogMode.ErrorsOnly)).tracer;
  assert.equal(errorsOnly.enabled(ZLinkMessageFlowOutcome.Dropped), true);
  assert.equal(errorsOnly.enabled(ZLinkMessageFlowOutcome.Received), false);

  const keyTransitions = makeTracer(diagnostics(ZLinkMessageFlowLogMode.KeyTransitions)).tracer;
  assert.equal(keyTransitions.enabled(ZLinkMessageFlowOutcome.Received), true);
  assert.equal(keyTransitions.enabled(ZLinkMessageFlowOutcome.Replied), true);
  assert.equal(keyTransitions.enabled(ZLinkMessageFlowOutcome.Dropped), true);
});

test('MFLOW-003/005 structured key=value line with label= is written to the separated log file', () => {
  const logDir = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-mflow-'));
  const logFile = path.join(logDir, 'flow.log');
  const { tracer } = makeTracer(diagnostics(ZLinkMessageFlowLogMode.KeyTransitions, {
    logFile,
    label: 'api'
  }));
  tracer.trace(receivedEvent());
  assert.equal(tracer.tracedCount, 1);
  const line = fs.readFileSync(logFile, 'utf8').trim();
  assert.match(line, /phase=received/);
  assert.match(line, /surface=channel/);
  assert.match(line, /label=api/);
  assert.match(line, /packet=EchoRequest/);
  assert.match(line, /channel=api/);
  assert.match(line, /corr=corr-1/);
});

test('MFLOW-004 observer offload delivers the event asynchronously', async () => {
  const events = [];
  class FlowObserver {
    onMessageFlow(event) {
      events.push(event);
    }
  }
  const { tracer } = makeTracer(diagnostics(ZLinkMessageFlowLogMode.KeyTransitions, {
    logFile: path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-mflow-obs-')), 'flow.log')
  }));
  const observed = makeTracer(
    diagnostics(ZLinkMessageFlowLogMode.KeyTransitions),
    undefined,
    FlowObserver
  ).tracer;
  assert.equal(events.length, 0, 'observer must not fire synchronously');
  observed.trace(receivedEvent());
  assert.equal(events.length, 0, 'observer is offloaded, not synchronous');
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(events.length, 1);
  assert.equal(events[0].outcome, 'succeeded');
  assert.equal(events[0].phase, 'received');
  assert.equal(events[0].correlationId, 'corr-1');
  void tracer;
});

test('MFLOW-004b success-path observer failures use message-flow-observer task', async () => {
  const failures = [];
  class ThrowingObserver {
    onMessageFlow() {
      throw new Error('success observer failed');
    }
  }
  const dispatch = { diagnostics: diagnostics(ZLinkMessageFlowLogMode.KeyTransitions) };
  const cell = createMessageFlowModeCell(dispatch);
  const ctx = {
    ...createDiagnosticsContext(dispatch, undefined, cell),
    messageFlowObserverType: ThrowingObserver
  };
  const tracer = new ZLinkMessageFlowTracer(ctx, {
    reportRuntimeTaskException(taskName, error) {
      failures.push({ taskName, error });
    }
  });

  tracer.trace(receivedEvent());
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(failures.length, 1);
  assert.equal(failures[0].taskName, 'message-flow-observer');
  assert.equal(failures[0].error.message, 'success observer failed');
  assert.equal(tracer.observerFailureCount, 1);
});

test('MFLOW-004c observer failure emits one bounded public runtime error event', async () => {
  const events = [];
  class ThrowingObserver {
    onMessageFlow() {
      throw new Error('observer failed');
    }
  }
  class RuntimeErrorSink {
    onRuntimeError(event) {
      events.push(event);
    }
  }
  const providerResolver = {
    get(type) {
      return type === RuntimeErrorSink ? new RuntimeErrorSink() : undefined;
    }
  };
  const dispatch = { diagnostics: diagnostics(ZLinkMessageFlowLogMode.KeyTransitions) };
  const cell = createMessageFlowModeCell(dispatch);
  const ctx = {
    ...createDiagnosticsContext(dispatch, providerResolver, cell),
    messageFlowObserverType: ThrowingObserver,
    runtimeErrorSinkType: RuntimeErrorSink
  };
  const tracer = new ZLinkMessageFlowTracer(ctx, silentSink());

  tracer.trace(receivedEvent());
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(tracer.observerFailureCount, 1);
  assert.equal(events.length, 1);
  assert.deepEqual(Object.keys(events[0]).sort(), [
    'eventId',
    'kind',
    'reason',
    'source',
    'timestamp'
  ]);
  assert.equal(events[0].eventId, 'zlink.runtime_error');
  assert.equal(events[0].kind, 'observer_failed');
  assert.equal(events[0].source, 'message_flow_observer');
  assert.equal(events[0].reason, 'Error: observer failed');
  assert.equal(events[0].timestamp instanceof Date, true);
});

test('MFLOW-009 live-mode cell toggles every reader without rebuilding the tracer', () => {
  const { tracer, cell } = makeTracer(diagnostics(ZLinkMessageFlowLogMode.Off));
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), false);
  cell.mode = ZLinkMessageFlowLogMode.Verbose;
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), true);
  cell.mode = ZLinkMessageFlowLogMode.Off;
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), false);
});

test('MFLOW-EXT flow sampling keeps or drops every event in one flow together', async () => {
  const events = [];
  class FlowObserver {
    onMessageFlow(event) {
      events.push(event);
    }
  }
  const { tracer } = makeTracer(
    diagnostics(ZLinkMessageFlowLogMode.KeyTransitions, {
      sampleRate: 0.5,
      logFile: path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-flow-sample-')), 'flow.log')
    }),
    undefined,
    FlowObserver
  );
  const flow = {
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 'Inbound'
  };

  tracer.trace({ ...receivedEvent(), ...flow });
  tracer.trace({
    ...receivedEvent(),
    ...flow,
    outcome: ZLinkMessageFlowOutcome.Dispatched
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.ok(events.length === 0 || events.length === 2, `sampled ${events.length} of 2 events`);
});

test('MFLOW-EXT create-if-absent keeps one flow across an async continuation', async () => {
  await new Promise((resolve, reject) => {
    setImmediate(async () => {
      try {
        const first = flowContext.currentOrCreateFlow();
        await Promise.resolve();
        const second = flowContext.currentOrCreateFlow();
        assert.equal(second.flowId, first.flowId);
        assert.equal(second.flowOrigin, first.flowOrigin);
        resolve();
      } catch (error) {
        reject(error);
      }
    });
  });
});

test('MFLOW-EXT channel wire and outbound trace use the same created flow', async () => {
  await new Promise((resolve, reject) => {
    setImmediate(async () => {
      const parts = channelEnvelope.encodeChannelEnvelopeParts(
        1,
        'api',
        'EchoRequest',
        { value: 'ping' }
      );
      try {
        const header = JSON.parse(Buffer.from(parts[0]).toString());
        const events = [];
        class FlowObserver {
          onMessageFlow(event) {
            events.push(event);
          }
        }
        const { tracer } = makeTracer(
          diagnostics(ZLinkMessageFlowLogMode.KeyTransitions, {
            logFile: path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-flow-wire-')), 'flow.log')
          }),
          undefined,
          FlowObserver
        );
        tracer.trace(receivedEvent());
        await new Promise((settled) => setImmediate(settled));
        assert.equal(events[0].flowId, header.flowId);
        assert.equal(header.flowOrigin, 3);
        assert.equal(events[0].flowOrigin, 'Application');
        resolve();
      } catch (error) {
        reject(error);
      } finally {
        channelEnvelope.closeMessages(parts);
      }
    });
  });
});

test('MFLOW-EXT Off host does not create channel or stream flow fields', async () => {
  await new Promise((resolve, reject) => {
    setImmediate(() => {
      const channelParts = channelEnvelope.encodeChannelEnvelopeParts(
        1,
        'api',
        'EchoRequest',
        { value: 'ping' },
        undefined,
        undefined,
        undefined,
        undefined,
        false
      );
      try {
        const channelHeader = JSON.parse(Buffer.from(channelParts[0]).toString());
        assert.equal(channelHeader.flowId, undefined);
        assert.equal(channelHeader.flowOrigin, undefined);

        let streamFrame;
        const factory = new ZLinkStreamFrameMessageFactory({
          flowCreationEnabled: () => false,
          messageFactory: {
            createTextMessage() { throw new Error('binary frame expected'); },
            createBinaryMessage(payload) {
              streamFrame = payload;
              return {};
            }
          }
        });
        factory.createJsonFrameMessage(
          streamProtocol.ZLinkStreamMessageKind.Send,
          'Push',
          new Map(),
          false,
          undefined,
          { value: 'push' }
        );
        const streamHeader = streamProtocol.decodeStreamFrame(streamFrame).header;
        assert.equal(streamHeader.flowId, undefined);
        assert.equal(streamHeader.flowOrigin, undefined);
        resolve();
      } catch (error) {
        reject(error);
      } finally {
        channelEnvelope.closeMessages(channelParts);
      }
    });
  });
});

test('MFLOW-EXT Off host preserves an inbound ambient flow on outbound wire', () => {
  const inbound = {
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 'Inbound'
  };
  flowContext.runWithFlow(inbound, () => {
    const parts = channelEnvelope.encodeChannelEnvelopeParts(
      1,
      'api',
      'EchoRequest',
      { value: 'ping' },
      undefined,
      undefined,
      undefined,
      undefined,
      false
    );
    try {
      const header = JSON.parse(Buffer.from(parts[0]).toString());
      assert.equal(header.flowId, inbound.flowId);
      assert.equal(header.flowOrigin, 1);
    } finally {
      channelEnvelope.closeMessages(parts);
    }
  });
});

test('MFLOW-010 stream correlation_id round-trips byte-identically across framework and connector codecs', () => {
  const correlationId = '1a2b';
  const requestSeq = 7n;
  const name = 'EchoRequest';

  const connectorBytes = protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    requestSeq,
    name,
    metadata: connector.ZlinkStreamMetadataMap.empty,
    correlationId
  });

  const frameworkDecoded = streamProtocol.decodeStreamHeader(connectorBytes);
  assert.equal(frameworkDecoded.correlationId, correlationId);
  assert.equal(frameworkDecoded.requestSeq, requestSeq);
  assert.equal(frameworkDecoded.name, name);

  const frameworkBytes = streamProtocol.encodeStreamHeader({
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    requestSeq,
    name,
    metadata: new Map(),
    correlationId
  });
  assert.deepEqual(Buffer.from(frameworkBytes), Buffer.from(connectorBytes));

  const connectorDecoded = protocolCodecs.ZlinkStreamHeaderCodec.decode(frameworkBytes);
  assert.equal(connectorDecoded.correlationId, correlationId);

  assert.notEqual((frameworkDecoded.flags & 0x08), 0, 'HasCorrelationId flag must be set');
});

test('MFLOW-010b correlation id survives alongside metadata (no offset overlap)', () => {
  const correlationId = 'deadbeef';
  const metadata = connector.ZlinkStreamMetadataMap.empty.withMany([['tenant', 'acme'], ['trace', 'on']]);

  const bytes = protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    requestSeq: 9n,
    name: 'WithMeta',
    metadata,
    correlationId
  });
  const roundTripped = protocolCodecs.ZlinkStreamHeaderCodec.decode(bytes);
  assert.equal(roundTripped.correlationId, correlationId);
  assert.equal(roundTripped.metadata.get('tenant'), 'acme');
  assert.equal(roundTripped.metadata.get('trace'), 'on');

  // framework codec decodes the metadata-bearing corr frame without losing either field.
  const frameworkDecoded = streamProtocol.decodeStreamHeader(bytes);
  assert.equal(frameworkDecoded.correlationId, correlationId);
  assert.equal(frameworkDecoded.metadata.get('tenant'), 'acme');
});

test('MFLOW-010c stream frame prefix and payload round-trip across framework and connector codecs', () => {
  const header = {
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 11n,
    name: 'FullFrame',
    metadata: new Map([['trace', 'frame-1']]),
    correlationId: 'corr-frame'
  };
  const payload = Buffer.from(JSON.stringify({ ok: true }));

  const frameworkFrame = streamProtocol.encodeStreamFrame(header, payload);
  const connectorFrame = protocolCodecs.ZlinkStreamFrameCodec.decode(frameworkFrame);
  const connectorHeader = protocolCodecs.ZlinkStreamHeaderCodec.decode(connectorFrame.header);

  assert.equal(connectorHeader.name, 'FullFrame');
  assert.equal(connectorHeader.requestSeq, 11n);
  assert.equal(connectorHeader.metadata.get('trace'), 'frame-1');
  assert.equal(connectorHeader.correlationId, 'corr-frame');
  assert.deepEqual(Buffer.from(connectorFrame.payload), payload);
  assert.deepEqual(
    Buffer.from(protocolCodecs.ZlinkStreamFrameCodec.encode(connectorFrame.header, connectorFrame.payload)),
    Buffer.from(frameworkFrame)
  );
});

test('MFLOW-010d framework and connector reject duplicate stream metadata keys', () => {
  const duplicateHeader = duplicateMetadataHeaderBytes();

  assert.throws(
    () => streamProtocol.decodeStreamHeader(duplicateHeader),
    /metadata key is duplicated/i
  );
  assert.throws(
    () => protocolCodecs.ZlinkStreamHeaderCodec.decode(duplicateHeader),
    /Duplicate metadata key/i
  );
});

test('MFLOW-011 control packets reject a correlation id', () => {
  assert.throws(() => protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Control,
    codec: connector.ZlinkStreamCodec.Raw,
    flags: connector.ZlinkStreamHeaderFlags.None,
    requestSeq: undefined,
    name: 'ctl',
    metadata: connector.ZlinkStreamMetadataMap.empty,
    correlationId: 'nope'
  }));
});

test('MFLOW-EXT-005/006 stream flow fields use mandatory marker and reject old or unknown formats', () => {
  const flowId = '018f2b63-9d4a-7abc-8def-0123456789ab';
  const encoded = protocolCodecs.ZlinkStreamHeaderCodec.encode({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'FlowEvent',
    metadata: connector.ZlinkStreamMetadataMap.empty,
    flowId,
    flowOrigin: 'Application'
  });
  assert.equal(encoded[0], 0xf2);
  assert.notEqual(encoded[3] & connector.ZlinkStreamHeaderFlags.HasFlowId, 0);
  const decoded = streamProtocol.decodeStreamHeader(encoded);
  assert.equal(decoded.flowId, flowId);
  assert.equal(decoded.flowOrigin, 'Application');

  assert.throws(() => protocolCodecs.ZlinkStreamHeaderCodec.decode(encoded.subarray(1)), /format marker/i);
  const unknownFlag = Uint8Array.from(encoded);
  unknownFlag[3] |= 0x20;
  assert.throws(() => protocolCodecs.ZlinkStreamHeaderCodec.decode(unknownFlag), /unknown mandatory|unknown stream header flag/i);
});

function duplicateMetadataHeaderBytes() {
  const name = Buffer.from('DupMeta');
  const key = Buffer.from('trace');
  const first = Buffer.from('one');
  const second = Buffer.from('two');
  const metadataLength = 1
    + 1 + key.length + 2 + first.length
    + 1 + key.length + 2 + second.length;
  const header = Buffer.alloc(4 + 8 + 1 + name.length + 2 + metadataLength);
  let offset = 0;
  header[offset++] = 0xf2;
  header[offset++] = connector.ZlinkStreamMessageKind.Request;
  header[offset++] = connector.ZlinkStreamCodec.Json;
  header[offset++] = connector.ZlinkStreamHeaderFlags.HasRequestSeq
    | connector.ZlinkStreamHeaderFlags.HasMetadata;
  header.writeBigUInt64BE(12n, offset);
  offset += 8;
  header[offset++] = name.length;
  name.copy(header, offset);
  offset += name.length;
  header.writeUInt16BE(metadataLength, offset);
  offset += 2;
  header[offset++] = 2;
  offset = writeMetadataEntry(header, offset, key, first);
  writeMetadataEntry(header, offset, key, second);
  return header;
}

function writeMetadataEntry(header, offset, key, value) {
  header[offset++] = key.length;
  key.copy(header, offset);
  offset += key.length;
  header.writeUInt16BE(value.length, offset);
  offset += 2;
  value.copy(header, offset);
  return offset + value.length;
}
