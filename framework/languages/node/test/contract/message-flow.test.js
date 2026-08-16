'use strict';

// MFLOW-xxx: message-flow tracing parity (mirrors C++/.NET/Java). Exercises the tracer's
// mode gating (zero-cost off), OpenTelemetry provider output, the live-mode toggle,
// and the stream correlation_id wire round-trip
// (framework <-> connector codecs byte-identical).

const assert = require('node:assert/strict');
const test = require('node:test');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');

const telemetryRecords = [];
let failTelemetryProvider = false;
const loggerProvider = new LoggerProvider({
  processors: [{
    onEmit(record) {
      if (failTelemetryProvider) throw new Error('logger provider failed');
      telemetryRecords.push(record);
    },
    forceFlush() { return Promise.resolve(); },
    shutdown() { return Promise.resolve(); }
  }]
});
logs.setGlobalLoggerProvider(loggerProvider);

const framework = require('../../packages/framework/dist/internal');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const { ZLinkStreamFrameMessageFactory } = require('../../packages/framework/dist/runtime/streams/stream-frame-factory');
const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkDispatchErrorReporter
} = require('../../packages/framework/dist/runtime/channels/dispatch-error-reporter');
const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');

const {
  ZLinkMessageFlowTracer,
  createDiagnosticsContext,
  createMessageFlowModeCell
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

function makeTracer(diagnosticsOptions, sink = silentSink()) {
  const dispatch = { diagnostics: diagnosticsOptions };
  const cell = createMessageFlowModeCell(dispatch);
  const ctx = createDiagnosticsContext(dispatch, undefined, cell);
  return { tracer: new ZLinkMessageFlowTracer(ctx, sink), cell };
}

test.beforeEach(() => {
  telemetryRecords.length = 0;
  failTelemetryProvider = false;
});

test.after(async () => loggerProvider.shutdown());

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
  const { tracer } = makeTracer(diagnostics('off'));
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), false);
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Dropped), false);
  tracer.trace(receivedEvent());
  assert.equal(tracer.tracedCount, 0);
});

test('MFLOW-002 mode ladder gates phases by severity', () => {
  const errorsOnly = makeTracer(diagnostics('errors')).tracer;
  assert.equal(errorsOnly.enabled(ZLinkMessageFlowOutcome.Dropped), true);
  assert.equal(errorsOnly.enabled(ZLinkMessageFlowOutcome.Received), false);

  const keyTransitions = makeTracer(diagnostics('normal')).tracer;
  assert.equal(keyTransitions.enabled(ZLinkMessageFlowOutcome.Received), true);
  assert.equal(keyTransitions.enabled(ZLinkMessageFlowOutcome.Replied), true);
  assert.equal(keyTransitions.enabled(ZLinkMessageFlowOutcome.Dropped), true);
});

test('MFLOW-003/005 standard logger provider receives the structured record', () => {
  const { tracer } = makeTracer(diagnostics('normal'));
  tracer.trace(receivedEvent());
  assert.equal(tracer.tracedCount, 1);
  assert.equal(telemetryRecords.length, 1);
  const record = telemetryRecords[0];
  assert.equal(record.eventName, 'zlink.message_flow');
  assert.equal(record.attributes.phase, 'received');
  assert.equal(record.attributes.surface, 'channel');
  assert.equal(record.attributes.packet_name, 'EchoRequest');
  assert.equal(record.attributes.channel_name, 'api');
  assert.equal(record.attributes.correlation_id, 'corr-1');
});

test('MFLOW-004 provider failures do not change the message operation', () => {
  const failures = [];
  const { tracer } = makeTracer(diagnostics('normal'), {
    reportRuntimeTaskException(taskName, error) {
      failures.push({ taskName, error });
    }
  });
  failTelemetryProvider = true;
  assert.doesNotThrow(() => tracer.trace(receivedEvent()));
  assert.equal(failures.length, 1);
  assert.equal(failures[0].taskName, 'logger-provider');
  assert.equal(failures[0].error.message, 'logger provider failed');
  assert.equal(tracer.providerFailureCount, 1);
});

test('dispatch errors record service-wire command and deepest handler cause', () => {
  const reporter = new ZLinkDispatchErrorReporter(
    undefined,
    undefined,
    silentSink(),
    {
      diagnostics: diagnostics('errors'),
      liveMode: { mode: 'errors' }
    }
  );
  reporter.report({
    surface: 'routeMeshChannel',
    messageKind: 'send',
    reason: 'handler_exception',
    action: 'drop',
    channelName: 'play',
    commandId: 34,
    error: new Error('Relocation owner committed, but target publication failed.', {
      cause: new TypeError('Location authority relocation envelope is missing.')
    })
  });

  assert.equal(telemetryRecords.length, 1);
  const attributes = telemetryRecords[0].attributes;
  assert.equal(attributes.command_id, 34);
  assert.equal(attributes.error_type, 'Error');
  assert.equal(
    attributes.error_message,
    'Relocation owner committed, but target publication failed.'
  );
  assert.equal(attributes.error_cause_type, 'TypeError');
  assert.equal(
    attributes.error_cause_message,
    'Location authority relocation envelope is missing.'
  );
});

test('MFLOW-009 live-mode cell toggles every reader without rebuilding the tracer', () => {
  const { tracer, cell } = makeTracer(diagnostics('off'));
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), false);
  cell.mode = 'detailed';
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), true);
  cell.mode = 'off';
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Received), false);
});

test('MFLOW-009 snapshots live mode for every transition in one ambient flow', () => {
  const { tracer, cell } = makeTracer(diagnostics('normal'));
  const inbound = {
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 'Inbound'
  };

  flowContext.runWithFlow(inbound, () => {
    tracer.trace(receivedEvent());
    cell.mode = 'off';
    assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Dispatched), true);
    tracer.trace({ ...receivedEvent(), outcome: ZLinkMessageFlowOutcome.Dispatched });
  });

  assert.equal(tracer.tracedCount, 2);
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Dispatched), false);
});

test('MFLOW-EXT flow sampling keeps or drops every event in one flow together', () => {
  const { tracer } = makeTracer(diagnostics('normal', { sampleRate: 0.5 }));
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
  assert.ok(
    telemetryRecords.length === 0 || telemetryRecords.length === 2,
    `sampled ${telemetryRecords.length} of 2 events`
  );
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
        const { tracer } = makeTracer(diagnostics('normal'));
        tracer.trace(receivedEvent());
        assert.equal(telemetryRecords[0].attributes.flow_id, header.flowId);
        assert.equal(header.flowOrigin, 3);
        assert.equal(telemetryRecords[0].attributes.flow_origin, 'Application');
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

test('MFLOW-EXT absent disabled flow does not create an ambient context', () => {
  const absent = flowContext.createInboundFlow(undefined, undefined, false);
  assert.equal(absent, undefined);
  flowContext.runWithFlow(absent, () => {
    assert.equal(flowContext.currentOrCreateFlow('Application', false), undefined);
  });
});

test('MFLOW-EXT explicit absent flow still suppresses a parent ambient context', () => {
  const inbound = {
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 'Inbound'
  };
  flowContext.runWithFlow(inbound, () => {
    flowContext.runWithFlow(undefined, () => {
      assert.equal(flowContext.currentOrCreateFlow('Application', false), undefined);
    });
    assert.equal(flowContext.currentOrCreateFlow('Application', false), inbound);
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
