import { parentPort, workerData } from 'node:worker_threads';
import { metrics, trace, ProxyTracerProvider, SpanStatusCode, type Tracer, type Counter } from '@opentelemetry/api';
import { logs, SeverityNumber, type Logger } from '@opentelemetry/api-logs';
import { initialLoggerProvider } from './message-flow-bootstrap';
import { TelemetryState, TelemetryCounter, TELEMETRY_STATE_BYTES, TELEMETRY_WORKER_AVAILABLE, type ZLinkTelemetryDelivery } from './message-flow-delivery';
import { COUNTER_UNITS, METRIC_NAMES } from './runtime-metrics';
import { ZLinkMeters } from '../../contracts/Eventing/Metrics';
import {
  DISPATCH_ERROR_EVENT_ID,
  TELEMETRY_INSTRUMENTATION_NAME,
  structuredLogBody,
  structuredLogAttributes,
  traceAttributes,
  type ZLinkTelemetryRecord
} from './message-flow-record';

const state = new Int32Array(workerData as SharedArrayBuffer, 0, TelemetryState.Length);
const counters = new BigUint64Array(workerData as SharedArrayBuffer, TELEMETRY_STATE_BYTES, TelemetryCounter.Length);
let logger: Logger | undefined;
let tracer: Tracer | undefined;
let overflow: Counter | undefined;
const overflowAttributes = { source: 'message_flow' };
let reportedOverflow = 0n;

function failed(role: TelemetryCounter.LoggerFailures | TelemetryCounter.TraceFailures, error: unknown): void {
  if (Atomics.add(counters, role, 1n) !== 0n || Atomics.load(counters, TelemetryCounter.FallbackFailures) !== 0n) return;
  try {
    const description = error instanceof Error ? `${error.name}: ${error.message}` : String(error);
    process.stderr.write(`ZLink telemetry ${role === TelemetryCounter.LoggerFailures ? 'logger' : 'trace'} provider failed: ${description}\n`, error => {
      if (error !== undefined && error !== null) Atomics.store(counters, TelemetryCounter.FallbackFailures, 1n);
    });
  } catch {
    Atomics.store(counters, TelemetryCounter.FallbackFailures, 1n);
  }
}

function deliver(record: ZLinkTelemetryRecord): void {
  if (logger !== undefined) {
    try {
      const severity = record.severity === 'warning' ? 'WARN'
        : record.eventId === DISPATCH_ERROR_EVENT_ID ? 'ERROR' : 'INFO';
      logger.emit({
        eventName: record.eventId,
        timestamp: record.timestamp,
        severityNumber: SeverityNumber[severity],
        severityText: severity,
        body: structuredLogBody(record),
        attributes: structuredLogAttributes(record)
      });
    } catch (error) {
      failed(TelemetryCounter.LoggerFailures, error);
    }
  }
  if (tracer === undefined) return;
  try {
    const span = tracer.startSpan(record.eventId, { attributes: traceAttributes(record) });
    if (record.eventId === DISPATCH_ERROR_EVENT_ID) {
      span.setStatus({ code: SpanStatusCode.ERROR, message: record.reason });
    }
    span.end();
  } catch (error) {
    failed(TelemetryCounter.TraceFailures, error);
  }
}

process.stderr.once('error', () => Atomics.store(counters, TelemetryCounter.FallbackFailures, 1n));
const hasLoggerProvider = logs.getLoggerProvider() !== initialLoggerProvider;
if (hasLoggerProvider) {
  try {
    logger = logs.getLogger(TELEMETRY_INSTRUMENTATION_NAME);
  } catch (error) {
    failed(TelemetryCounter.LoggerFailures, error);
  }
}
let hasTraceProvider = false;
try {
  const provider = trace.getTracerProvider();
  tracer = provider instanceof ProxyTracerProvider
    ? provider.getDelegateTracer(TELEMETRY_INSTRUMENTATION_NAME)
    : provider.getTracer(TELEMETRY_INSTRUMENTATION_NAME);
  hasTraceProvider = tracer !== undefined;
} catch (error) {
  // The no-provider proxy returns undefined. A delegate throwing getTracer is
  // still a registered provider, and its failure stays in this worker.
  hasTraceProvider = true;
  failed(TelemetryCounter.TraceFailures, error);
}

// enabled() is an event filter, not a provider-registration test. In particular,
// an ERROR-only processor may reject an unspecified-severity enabled() query.
Atomics.store(state, TelemetryState.Enabled,
  hasLoggerProvider || hasTraceProvider ? 1 : 0);
parentPort!.postMessage(TELEMETRY_WORKER_AVAILABLE);
parentPort!.on('message', (batch: ZLinkTelemetryDelivery<ZLinkTelemetryRecord>[]) => {
  for (const item of batch) {
    Atomics.sub(state, TelemetryState.Pending, 1);
    if (Atomics.load(new Int32Array(item.lease), 0) === 0) deliver(item.record);
  }
  const dropped = Atomics.load(counters, TelemetryCounter.Overflow);
  if (dropped !== reportedOverflow) {
    try {
      overflow ??= metrics.getMeter(ZLinkMeters.Framework).createCounter(
        METRIC_NAMES.ObservabilityEventsOverflow,
        { unit: COUNTER_UNITS[METRIC_NAMES.ObservabilityEventsOverflow] }
      );
      overflow.add(Number(dropped - reportedOverflow), overflowAttributes);
    } catch (error) {
      failed(TelemetryCounter.TraceFailures, error);
    }
    reportedOverflow = dropped;
  }
  parentPort!.postMessage(TELEMETRY_WORKER_AVAILABLE);
});
