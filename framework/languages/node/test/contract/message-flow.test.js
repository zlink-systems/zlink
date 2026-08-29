'use strict';

// MFLOW-xxx: message-flow tracing parity (mirrors C++/.NET/Java). Exercises the tracer's
// mode gating (zero-cost off), OpenTelemetry provider output, the live-mode toggle,
// and the stream correlation_id wire round-trip
// (framework <-> connector codecs byte-identical).

const assert = require('node:assert/strict');
const test = require('node:test');
const { trace } = require('@opentelemetry/api');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');

const telemetryRecords = [];
const traceRecords = [];
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
trace.setGlobalTracerProvider({
  getTracer() {
    return {
      startSpan(eventName, options) {
        traceRecords.push({ eventName, attributes: options.attributes });
        return { setStatus() {}, end() {} };
      }
    };
  }
});

const framework = require('../../packages/framework/dist/internal');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const { ZLinkStreamFrameMessageFactory } = require('../../packages/framework/dist/runtime/streams/stream-frame-factory');
const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkChannelOutboundOperations
} = require('../../packages/framework/dist/runtime/channels/channel-outbound-operations');
const {
  ZLinkChannelDispatchPipeline
} = require('../../packages/framework/dist/runtime/channels/channel-dispatch-pipeline');
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
  Admitted: 'admitted',
  Dispatched: 'dispatched',
  Completed: 'completed',
  Replied: 'replied',
  Dropped: 'dropped',
  Backpressured: 'backpressured'
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
  traceRecords.length = 0;
  failTelemetryProvider = false;
});

test.after(async () => loggerProvider.shutdown());

test('message-flow async and sync controls complete before observation', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });

  await host.setMessageFlowModeAsync('normal');
  assert.equal(host.messageFlowMode(), 'normal');

  host.setMessageFlowMode('detailed');
  assert.equal(host.messageFlowMode(), 'detailed');
});

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

test('message-flow begin reuses its gated trace point instead of allocating per event', () => {
  const { tracer, cell } = makeTracer(diagnostics('normal'));
  const first = tracer.begin(ZLinkMessageFlowOutcome.Received);
  const second = tracer.begin(ZLinkMessageFlowOutcome.Admitted);
  assert.strictEqual(first, second);

  cell.mode = 'detailed';
  assert.notStrictEqual(tracer.begin(ZLinkMessageFlowOutcome.Received), first);
  cell.mode = 'off';
  assert.equal(tracer.begin(ZLinkMessageFlowOutcome.Received), undefined);
});

test('message-flow begin rejects a sampled-out event before its trace DTO is built', () => {
  const { tracer } = makeTracer(diagnostics('normal', { sampleRate: 0 }));
  assert.equal(
    tracer.begin(
      ZLinkMessageFlowOutcome.Received,
      undefined,
      '018f2b63-9d4a-7abc-8def-0123456789ab'
    ),
    undefined
  );
});

test('MFLOW-003/005 standard logger provider receives the structured record', () => {
  const { tracer } = makeTracer(diagnostics('normal'));
  tracer.trace(receivedEvent());
  assert.equal(tracer.tracedCount, 1);
  assert.equal(telemetryRecords.length, 1);
  const record = telemetryRecords[0];
  assert.equal(record.eventName, 'zlink.message_flow');
  assert.match(record.body, /^zlink flow: event=zlink\.message_flow /);
  assert.equal(record.attributes.event_id, 'zlink.message_flow');
  assert.equal(record.attributes.phase, 'received');
  assert.equal(record.attributes.surface, 'channel');
  assert.equal(record.attributes.packet_name, 'EchoRequest');
  assert.equal(record.attributes.channel_name, 'api');
  assert.equal(record.attributes.channel_route_kind, 'client_server');
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
  const attributes = traceRecords[0].attributes;
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

test('MFLOW-009 reads live mode independently at every transition in one ambient flow', () => {
  const { tracer, cell } = makeTracer(diagnostics('normal'));
  const inbound = {
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 'Inbound'
  };

  flowContext.runWithFlow(inbound, () => {
    tracer.trace(receivedEvent());
    cell.mode = 'off';
    assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Dispatched), false);
    tracer.trace({ ...receivedEvent(), outcome: ZLinkMessageFlowOutcome.Dispatched });
  });

  assert.equal(tracer.tracedCount, 1);
  assert.equal(tracer.enabled(ZLinkMessageFlowOutcome.Dispatched), false);
});

test('spec 26 phases and outcomes use the closed vocabulary', () => {
  const { tracer } = makeTracer(diagnostics('normal', { sampleRate: 0 }));
  const phases = [
    ZLinkMessageFlowOutcome.Admitted,
    ZLinkMessageFlowOutcome.Completed,
    ZLinkMessageFlowOutcome.Backpressured
  ];
  for (const outcome of phases) {
    tracer.trace({ ...receivedEvent(), outcome });
  }

  assert.deepEqual(
    telemetryRecords.map((record) => record.attributes.phase),
    ['backpressured']
  );
  assert.equal(telemetryRecords[0].attributes.outcome, 'backpressured');
  assert.equal(traceRecords[0].attributes.outcome, 'backpressured');
});

test('request failure terminals remain visible at Errors level', () => {
  const { tracer } = makeTracer(diagnostics('errors', { sampleRate: 0 }));
  tracer.trace({
    ...receivedEvent(),
    outcome: 'replyReceived',
    result: 'failed'
  });
  assert.equal(telemetryRecords.length, 1);
  assert.equal(telemetryRecords[0].attributes.phase, 'reply_received');
  assert.equal(telemetryRecords[0].attributes.outcome, 'failed');
});

test('spec 26 maps every added surface and control without admitting publish as a flow kind', () => {
  const { tracer } = makeTracer(diagnostics('normal'));
  for (const surface of ['node', 'actorRelocation', 'classicFanout']) {
    tracer.trace({
      ...receivedEvent(),
      surface,
      messageKind: surface === 'actorRelocation' ? 'control' : 'send'
    });
  }
  assert.deepEqual(
    telemetryRecords.map((record) => [record.attributes.surface, record.attributes.message_kind]),
    [['node', 'send'], ['actor_relocation', 'control'], ['classic_fanout', 'send']]
  );
  assert.throws(
    () => tracer.trace({ ...receivedEvent(), messageKind: 'publish' }),
    /fanout publish must not create/i
  );
});

test('spec 26 structured log projection uses only the exact keys', () => {
  const { tracer } = makeTracer(diagnostics('detailed', { includeMessageSizes: true }));
  tracer.trace({
    ...receivedEvent(),
    channelRouteKind: 'client_server',
    serverRid: 'server-1',
    targetRid: 'target-1',
    messageSize: 42,
    durationSeconds: 0.125
  });
  const keys = Object.keys(telemetryRecords[0].attributes).sort();
  //  기록의 attribute 이름은 Message flow tracing 3.2가 고정한다. `zlink flow:` 본문의
  //  축약 key(`kind`, `mesh` …)는 별도 집합이며 본문 문자열에만 쓴다.
  const allowed = [
    'action', 'activation_state', 'actor_id', 'channel_name', 'channel_route_kind',
    'correlation_id', 'duration_seconds', 'event_id', 'flow_id', 'flow_origin',
    'instance_spot_type', 'mesh_name', 'message_kind', 'message_size_bytes', 'outcome',
    'packet_name', 'phase', 'reason', 'server_rid', 'source_rid', 'spot_id', 'surface',
    'target_rid', 'timestamp', 'topic'
  ];
  assert.deepEqual(keys, allowed.filter((key) => keys.includes(key)).sort());
  assert.equal(telemetryRecords[0].attributes.server_rid, 'server-1');
  assert.equal(traceRecords[0].attributes.duration_seconds, 0.125);
});

test('spec 26 flow-less sampling does not create a flow context and backpressure bypasses sampling', () => {
  const { tracer } = makeTracer(diagnostics('normal', { sampleRate: 0 }));
  assert.equal(flowContext.currentFlowContext(), undefined);
  tracer.trace({ ...receivedEvent(), sourceMeshGeneration: 17n });
  assert.equal(flowContext.currentFlowContext(), undefined);
  tracer.trace({
    ...receivedEvent(),
    outcome: ZLinkMessageFlowOutcome.Backpressured,
    result: 'backpressured',
    sourceMeshGeneration: 17n
  });
  assert.equal(telemetryRecords.length, 1);
  assert.equal(telemetryRecords[0].attributes.flow_id, undefined);
});

test('dispatch reporter Off gate skips trace formatting and trace-only counters', () => {
  const reporter = new ZLinkDispatchErrorReporter(
    undefined,
    undefined,
    silentSink(),
    { diagnostics: diagnostics('off'), liveMode: { mode: 'off' } }
  );
  const error = {};
  Object.defineProperty(error, 'toString', {
    value() { throw new Error('error formatting must remain behind the Off gate'); }
  });
  assert.doesNotThrow(() => reporter.report({
    surface: 'channel',
    messageKind: 'send',
    reason: 'handler_exception',
    action: 'drop',
    error
  }));
  assert.equal(reporter.reportedCount, 0);
  assert.equal(telemetryRecords.length, 0);
});

test('channel request completion owner records one terminal for failure, cancellation, and shutdown', async () => {
  const cases = [
    {
      expected: 'failed',
      sockets: {
        async awaitClientDealerForOutbound() { return undefined; },
        hasKnownClientServerTargets() { return false; }
      }
    },
    {
      expected: 'cancelled',
      signal: AbortSignal.abort(new Error('cancelled')),
      sockets: {
        async awaitClientDealerForOutbound() { throw new Error('must not await'); },
        hasKnownClientServerTargets() { return false; }
      }
    },
    {
      expected: 'failed',
      sockets: {
        async awaitClientDealerForOutbound() {
          throw new framework.ZLinkFrameworkException(
            framework.ZLinkFrameworkErrorKind.DeadlineExceeded,
            'request deadline'
          );
        },
        hasKnownClientServerTargets() { return true; }
      }
    },
    {
      expected: 'shutdown',
      sockets: {
        async awaitClientDealerForOutbound() {
          throw new framework.ZLinkFrameworkException(
            framework.ZLinkFrameworkErrorKind.ShuttingDown,
            'runtime shutdown'
          );
        },
        hasKnownClientServerTargets() { return false; }
      }
    }
  ];

  for (const scenario of cases) {
    const events = [];
    const operations = new ZLinkChannelOutboundOperations(
      scenario.sockets,
      undefined,
      {
        flowCreationEnabled() { return false; },
        beginOutbound(outcome) {
          return { trace(event) { events.push({ outcome, ...event }); } };
        }
      }
    );
    await assert.rejects(() => operations.request(
      'api',
      'EchoRequest',
      { value: 'ping' },
      10,
      scenario.signal
    ));
    const terminals = events.filter((event) => event.outcome === 'replyReceived');
    assert.equal(terminals.length, 1);
    assert.equal(terminals[0].result, scenario.expected);
  }
});

test('channel request completion owner records success and in-flight cancellation once', async () => {
  const successEvents = [];
  const successDealer = {
    async request(parts) {
      const messages = parts.map((part) =>
        typeof part.data === 'function' ? part : bindingMessage(part));
      const header = channelEnvelope.decodeChannelHeader(messages);
      channelEnvelope.closeMessages(parts);
      return channelEnvelope.encodeChannelReplyParts(header, { ok: true }).map((part) =>
        typeof part.data === 'function' ? part : bindingMessage(part));
    }
  };
  const success = new ZLinkChannelOutboundOperations(
    {
      async awaitClientDealerForOutbound() { return successDealer; },
      hasKnownClientServerTargets() { return true; },
      selectedClientServerRid() { return 'server-1'; }
    },
    undefined,
    {
      flowCreationEnabled() { return false; },
      beginOutbound(outcome) {
        return { trace(event) { successEvents.push({ outcome, ...event }); } };
      }
    }
  );
  assert.deepEqual(
    await success.request('api', 'EchoRequest', { value: 'ping' }, 100),
    { ok: true }
  );
  assert.deepEqual(
    successEvents.filter((event) => event.outcome === 'replyReceived').map((event) => event.result),
    ['succeeded']
  );

  const cancelledEvents = [];
  const controller = new AbortController();
  const pendingDealer = {
    request(parts) {
      channelEnvelope.closeMessages(parts);
      return new Promise(() => {});
    }
  };
  const pending = new ZLinkChannelOutboundOperations(
    {
      async awaitClientDealerForOutbound() { return pendingDealer; },
      hasKnownClientServerTargets() { return true; },
      selectedClientServerRid() { return 'server-2'; }
    },
    undefined,
    {
      flowCreationEnabled() { return false; },
      beginOutbound(outcome) {
        return { trace(event) { cancelledEvents.push({ outcome, ...event }); } };
      }
    }
  );
  const operation = pending.request(
    'api', 'EchoRequest', { value: 'ping' }, 100, controller.signal
  );
  controller.abort();
  await assert.rejects(operation, /aborted/i);
  assert.deepEqual(
    cancelledEvents.filter((event) => event.outcome === 'replyReceived').map((event) => event.result),
    ['cancelled']
  );
});

test('route outbound tracing records the destination as target_rid', async () => {
  const events = [];
  const operations = new ZLinkChannelOutboundOperations(
    {
      routeRouter() {
        return {
          async send(_targetRid, parts) {
            channelEnvelope.closeMessages(parts);
          }
        };
      }
    },
    undefined,
    {
      flowCreationEnabled() { return false; },
      beginOutbound(outcome) {
        return { trace(event) { events.push({ outcome, ...event }); } };
      }
    }
  );

  await operations.routeSubmit('mesh', 'target-node', 'Push', { value: 'ping' });

  assert.equal(events.length, 1);
  assert.equal(events[0].targetRid, 'target-node');
  assert.equal(events[0].sourceRid, undefined);
});

test('classic fanout omits normal delivery flow and reports only subscriber-local no-handler', async () => {
  const flows = [];
  const errors = [];
  const pipeline = new ZLinkChannelDispatchPipeline({
    channelName: 'events',
    surface: 'channel',
    dispatchErrors: {
      flow: {
        flowCreationEnabled() { return false; },
        begin() {
          return { trace(event) { flows.push(event); } };
        }
      },
      report(event) { errors.push(event); }
    }
  });
  await pipeline.dispatchOneWay({
    fields: {
      messageKind: 'publish',
      packetName: 'Changed',
      topic: 'orders'
    },
    envelope: {},
    context: {}
  });
  assert.equal(flows.length, 0);
  assert.equal(errors.length, 1);
  assert.deepEqual(
    {
      surface: errors[0].surface,
      messageKind: errors[0].messageKind,
      reason: errors[0].reason,
      action: errors[0].action,
      channelRouteKind: errors[0].channelRouteKind
    },
    {
      surface: 'classicFanout',
      messageKind: 'send',
      reason: 'no_handler',
      action: 'drop',
      channelRouteKind: undefined
    }
  );
});

test('channel one-way flow records queue admission, handler start, and terminal completion in order', async () => {
  const phases = [];
  const parts = channelEnvelope.encodeChannelEnvelopeParts(
    3,
    'api',
    'Push',
    { value: 1 },
    undefined,
    undefined,
    undefined,
    undefined,
    false
  ).map((part) => typeof part.data === 'function' ? part : bindingMessage(part));
  const pipeline = new ZLinkChannelDispatchPipeline({
    channelName: 'api',
    surface: 'channel',
    dispatchErrors: {
      flow: {
        flowCreationEnabled() { return false; },
        begin() {
          return { trace(event) { phases.push(event.outcome); } };
        }
      },
      report(error) { assert.fail(`unexpected dispatch error: ${error.reason}`); }
    }
  });
  await pipeline.dispatchOneWay({
    fields: { messageKind: 'send', packetName: 'Push' },
    envelope: channelEnvelope.decodeChannelEnvelope(parts),
    handler: { handle() {} },
    context: {}
  });
  channelEnvelope.closeMessages(parts);
  assert.deepEqual(phases, ['received', 'admitted', 'dispatched', 'completed']);
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

test('MFLOW-EXT outbound flow scope is call-scoped and keeps one flow inside it', async () => {
  await new Promise((resolve, reject) => {
    setImmediate(async () => {
      try {
        // Spec 27 §4/§6: a top-level outbound installs its flow only for the
        // duration of the call. Inside the scope every read observes the
        // same flow, including across async continuations.
        let first;
        let second;
        await flowContext.runWithOutboundFlow(true, async () => {
          first = flowContext.currentOrCreateFlow();
          await Promise.resolve();
          second = flowContext.currentOrCreateFlow();
        });
        assert.equal(second.flowId, first.flowId);
        assert.equal(second.flowOrigin, first.flowOrigin);
        // After the call the ambient application context is clean: the flow
        // must not leak into unrelated work (the former enterWith install).
        assert.equal(flowContext.currentFlowContext(), undefined);
        const unrelated = flowContext.currentOrCreateFlow();
        assert.notEqual(unrelated.flowId, first.flowId);
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
      let parts;
      try {
        // The outbound entry points wrap encode + trace in one call-scoped
        // flow (runWithOutboundFlow); wire fields and the source-side trace
        // must agree without installing anything into the caller's context.
        let header;
        flowContext.runWithOutboundFlow(true, () => {
          parts = channelEnvelope.encodeChannelEnvelopeParts(
            1,
            'api',
            'EchoRequest',
            { value: 'ping' }
          );
          header = JSON.parse(Buffer.from(parts[0]).toString());
          const { tracer } = makeTracer(diagnostics('normal'));
          tracer.trace(receivedEvent());
        });
        assert.equal(telemetryRecords[0].attributes.flow_id, header.flowId);
        assert.equal(header.flowOrigin, 3);
        // Spec 27 §3 value set: telemetry emits lowercase origins in every
        // language even though the wire enum stays numeric.
        assert.equal(telemetryRecords[0].attributes.flow_origin, 'application');
        // The scope did not leak into the caller's ambient context.
        assert.equal(flowContext.currentFlowContext(), undefined);
        resolve();
      } catch (error) {
        reject(error);
      } finally {
        if (parts !== undefined) channelEnvelope.closeMessages(parts);
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

test('MFLOW-EXT Off host suppresses an inbound ambient flow on outbound wire', () => {
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
      assert.equal(header.flowId, undefined);
      assert.equal(header.flowOrigin, undefined);
    } finally {
      channelEnvelope.closeMessages(parts);
    }
  });
});

test('MFLOW-EXT spec 27 \u00a74 Off decode neither validates nor keeps inbound channel flow fields', async () => {
  await new Promise((resolve, reject) => {
    setImmediate(() => {
      const parts = channelEnvelope.encodeChannelEnvelopeParts(
        1,
        'api',
        'EchoRequest',
        { value: 'ping' }
      );
      try {
        const onHeader = JSON.parse(Buffer.from(parts[0]).toString());
        assert.equal(typeof onHeader.flowId, 'string');

        // Off strips the flow pair from the decoded header, so replies and
        // forwarded messages built from it carry no flow fields.
        const offEnvelope = channelEnvelope.decodeChannelEnvelope(
          [{ data: () => Buffer.from(parts[0]) }, { data: () => Buffer.from(parts[1]) }],
          undefined,
          false
        );
        assert.equal(offEnvelope.header.flowId, undefined);
        assert.equal(offEnvelope.header.flowOrigin, undefined);
        assert.equal(offEnvelope.header.correlationId, onHeader.correlationId);

        const replyParts = channelEnvelope.encodeChannelReplyParts(offEnvelope.header, { ok: true });
        const replyHeader = JSON.parse(Buffer.from(replyParts[0]).toString());
        assert.equal(replyHeader.flowId, undefined);
        assert.equal(replyHeader.flowOrigin, undefined);
        assert.equal(replyHeader.correlationId, onHeader.correlationId);
        channelEnvelope.closeMessages(replyParts);

        // Off skips flow validation entirely: a malformed flowId no longer
        // rejects the envelope (spec 27 \u00a74), while an enabled decode still does.
        const malformed = JSON.parse(Buffer.from(parts[0]).toString());
        malformed.flowId = 'not-a-uuid';
        const malformedParts = [
          { data: () => Buffer.from(JSON.stringify(malformed)) },
          { data: () => Buffer.from(parts[1]) }
        ];
        const decodedOff = channelEnvelope.decodeChannelEnvelope(malformedParts, undefined, false);
        assert.equal(decodedOff.header.flowId, undefined);
        assert.throws(() => channelEnvelope.decodeChannelEnvelope(malformedParts, undefined, true), /UUIDv7/);
        resolve();
      } catch (error) {
        reject(error);
      } finally {
        channelEnvelope.closeMessages(parts);
      }
    });
  });
});

test('MFLOW-EXT spec 27 \u00a74 Off stream reply preserves correlation but not flow fields', () => {
  const requestHeader = {
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 7n,
    name: 'EchoRequest',
    metadata: new Map(),
    correlationId: 'corr-1',
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 'Inbound'
  };

  let offFrame;
  const offFactory = new ZLinkStreamFrameMessageFactory({
    flowCreationEnabled: () => false,
    messageFactory: {
      createTextMessage() { throw new Error('binary frame expected'); },
      createBinaryMessage(payload) {
        offFrame = payload;
        return {};
      }
    }
  });
  offFactory.createJsonReplyFrameMessage(
    requestHeader,
    streamProtocol.ZLinkStreamMessageKind.Response,
    new Map(),
    false,
    { ok: true }
  );
  const offHeader = streamProtocol.decodeStreamHeader(streamProtocol.decodeStreamFrame(offFrame).header);
  assert.equal(offHeader.correlationId, 'corr-1');
  assert.equal(offHeader.flowId, undefined);
  assert.equal(offHeader.flowOrigin, undefined);

  let onFrame;
  const onFactory = new ZLinkStreamFrameMessageFactory({
    flowCreationEnabled: () => true,
    messageFactory: {
      createTextMessage() { throw new Error('binary frame expected'); },
      createBinaryMessage(payload) {
        onFrame = payload;
        return {};
      }
    }
  });
  onFactory.createJsonReplyFrameMessage(
    requestHeader,
    streamProtocol.ZLinkStreamMessageKind.Response,
    new Map(),
    false,
    { ok: true }
  );
  const onHeader = streamProtocol.decodeStreamHeader(streamProtocol.decodeStreamFrame(onFrame).header);
  assert.equal(onHeader.flowId, requestHeader.flowId);
  assert.equal(onHeader.flowOrigin, 'Inbound');
});

test('MFLOW-EXT spec 27 \u00a74 Off stream decode skips and strips malformed flow fields', () => {
  const valid = streamProtocol.encodeStreamHeader({
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 9n,
    name: 'EchoRequest',
    metadata: new Map(),
    correlationId: 'corr-9',
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 'Inbound'
  });
  const malformed = Buffer.from(valid);
  malformed.fill(0x78, malformed.length - 37, malformed.length - 1);
  malformed[malformed.length - 1] = 0xff;

  assert.throws(() => streamProtocol.decodeStreamHeader(malformed, true), /flow id|flow origin/i);
  const decodedOff = streamProtocol.decodeStreamHeader(malformed, false);
  assert.equal(decodedOff.correlationId, 'corr-9');
  assert.equal(decodedOff.flowId, undefined);
  assert.equal(decodedOff.flowOrigin, undefined);
  assert.equal(
    decodedOff.flags & streamProtocol.ZLinkStreamHeaderFlags.HasFlowId,
    0
  );
});

test('MFLOW-EXT absent disabled flow does not create an ambient context', () => {
  const absent = flowContext.createInboundFlow(undefined, undefined, false);
  assert.equal(absent, undefined);
  assert.equal(
    flowContext.createInboundFlow(
      '018f2b63-9d4a-7abc-8def-0123456789ab',
      'Inbound',
      false
    ),
    undefined,
    'Off must not read an inbound wire flow into async-local context'
  );
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

function bindingMessage(payload) {
  const bytes = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  return {
    data() { return bytes; },
    toBytes() { return bytes; },
    close() {}
  };
}

// ---------------------------------------------------------------------------
// Malformed channel envelope terminals (audit-r5 carryover): a malformed
// request receives a protocol error reply plus a
// zlink.dispatch_error(invalid_frame/reply_error) record; a malformed one-way
// frame is dropped with invalid_frame/drop (spec 26 §2.2/§3.1, spec 27 §7).
// ---------------------------------------------------------------------------

const {
  ZLinkChannelRequestDispatcher
} = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const boundSessionWire = require('../../packages/framework/dist/runtime/actors/bound-session-wire');

function makeDispatchErrorReporter() {
  return new ZLinkDispatchErrorReporter(
    undefined,
    undefined,
    silentSink(),
    {
      diagnostics: diagnostics('errors'),
      liveMode: { mode: 'errors' },
      sourceMeshGeneration: 1n
    }
  );
}

function malformedEnvelopeParts(headerFields) {
  return [
    bindingMessage(JSON.stringify({
      formatMarker: 0xf2,
      channelName: 'api',
      messageName: 'EchoRequest',
      contentType: 'application/json',
      deadline: null,
      topic: null,
      ...headerFields
    })),
    bindingMessage('{}')
  ];
}

test('MFLOW-MAL malformed request envelope replies protocol error and records invalid_frame/reply_error', async () => {
  const dispatcher = new ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: makeDispatchErrorReporter(),
    handlers: new Map()
  });
  const replies = [];
  const router = {
    reply() {
      const operation = {
        message(part) {
          replies.push(part);
          return operation;
        },
        submit() {}
      };
      return operation;
    }
  };

  await dispatcher.dispatch({
    // flowId without flowOrigin is a protocol error (spec 27 §3).
    parts: malformedEnvelopeParts({
      kind: 1,
      correlationId: 'corr-malformed',
      flowId: '01890000-0000-7000-8000-000000000001'
    }),
    routingId: 'peer-1',
    requestSeq: 9n
  }, router);

  assert.equal(replies.length >= 1, true);
  const replyHeader = JSON.parse(Buffer.from(replies[0]).toString());
  assert.equal(replyHeader.kind, 5);
  assert.equal(replyHeader.errorCode, 'protocol_error'); // ProtocolError
  assert.equal(replyHeader.correlationId, 'corr-malformed');

  assert.equal(traceRecords.length, 1);
  const attributes = traceRecords[0].attributes;
  assert.equal(attributes.event_id, 'zlink.dispatch_error');
  assert.equal(attributes.reason, 'invalid_frame');
  assert.equal(attributes.action, 'reply_error');
  assert.equal(attributes.correlation_id, 'corr-malformed');
  // Spec 27 §7: an incomplete flow pair is not carried and no fresh flow id
  // is fabricated for the failure record.
  assert.equal(attributes.flow_id, undefined);
});

test('MFLOW-MAL malformed one-way envelope drops and records invalid_frame/drop', async () => {
  const dispatcher = new ZLinkChannelRequestDispatcher({
    channelName: 'api',
    dispatchErrors: makeDispatchErrorReporter(),
    handlers: new Map()
  });
  let replied = false;
  const router = {
    reply() {
      replied = true;
      const operation = { message() { return operation; }, submit() {} };
      return operation;
    }
  };

  await dispatcher.dispatch({
    parts: malformedEnvelopeParts({
      kind: 3,
      correlationId: null,
      flowId: '01890000-0000-7000-8000-000000000001'
    }),
    routingId: 'peer-1',
    requestSeq: null
  }, router);

  assert.equal(replied, false);
  assert.equal(traceRecords.length, 1);
  const attributes = traceRecords[0].attributes;
  assert.equal(attributes.event_id, 'zlink.dispatch_error');
  assert.equal(attributes.reason, 'invalid_frame');
  assert.equal(attributes.action, 'drop');
});

test('MFLOW-WIRE relay JSON writes the spec lowercase flow_origin and decodes it back', () => {
  // Spec 27 §3: the wire spelling of flow_origin is lowercase; the
  // capitalized value is only the internal TypeScript union.
  const payload = boundSessionWire.encodeRemoteBoundSessionSendPayload({
    actorId: 'actor-1',
    message: { v: 1 },
    metadata: new Map(),
    flowId: '01890000-0000-7000-8000-000000000001',
    flowOrigin: 'Application'
  });
  assert.equal(payload.flowOrigin, 'application');

  const decoded = boundSessionWire.decodeRemoteBoundSessionSendPayload(
    JSON.parse(JSON.stringify(payload))
  );
  assert.equal(decoded.flowOrigin, 'Application');
  assert.equal(decoded.flowId, '01890000-0000-7000-8000-000000000001');

  // The decoder accepts only the spec spelling (repo-internal wire, deployed
  // atomically): a legacy capitalized value is not readable as a flow origin.
  const legacy = JSON.parse(JSON.stringify(payload));
  legacy.flowOrigin = 'Application';
  assert.equal(boundSessionWire.decodeRemoteBoundSessionSendPayload(legacy).flowOrigin, undefined);
});
