'use strict';

const fs = require('node:fs');
const { isMainThread, parentPort } = require('node:worker_threads');
const { trace, metrics } = require('@opentelemetry/api');
const { logs, createNoopLogger } = require('@opentelemetry/api-logs');
const mode = process.env.ZLINK_TEST_TELEMETRY_CASE;
const recordPath = process.env.ZLINK_TEST_TELEMETRY_RECORDS;
if (!isMainThread && mode === 'throwing-preload') throw new Error('telemetry preload failed');

function record(kind, value) {
  fs.appendFileSync(recordPath, JSON.stringify({ kind, thread: isMainThread ? 'main' : 'worker', value }) + '\n');
}

function barrier(path) {
  const deadline = Date.now() + 15000;
  while (!fs.existsSync(path)) {
    if (Date.now() >= deadline) throw new Error('Telemetry fixture barrier was not released.');
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 5);
  }
}

if (mode === 'blocked' || mode === 'blocked-trace' || mode === 'overflow-width') {
  metrics.setGlobalMeterProvider({ getMeter(meterName) { return {
    createCounter(name, options) { return { add(value, attributes) {
      record('metric', { meterName, name, unit: options.unit, value, attributes });
    } }; }
  }; } });
}

if (!isMainThread && mode === 'bootstrap') {
  record('bootstrap-entered');
  parentPort.postMessage('unrelated application preload message');
  barrier(process.env.ZLINK_TEST_TELEMETRY_GATE);
}

if (mode !== 'none' && mode !== 'common-helper' && !(mode === 'main-only' && !isMainThread)) {
  if (mode !== 'trace' && mode !== 'blocked-trace' && mode !== 'trace-init-failure') {
    if (mode === 'error-filter') {
      const { LoggerProvider } = require('@opentelemetry/sdk-logs');
      const provider = new LoggerProvider({ processors: [{
        enabled(options) { return options.severityNumber >= 17; },
        onEmit(value) { record('log', { eventName: value.eventName, attributes: value.attributes, body: value.body }); },
        forceFlush() { return Promise.resolve(); }, shutdown() { return Promise.resolve(); }
      }] });
      logs.setGlobalLoggerProvider(provider);
      record('unspecified-enabled', logs.getLogger('filter-control').enabled());
    } else {
      logs.setGlobalLoggerProvider({ getLogger() {
        if (mode === 'logger-init-failure') throw new TypeError('logger initialization failed');
        if (mode === 'registered-noop') return createNoopLogger();
        return {
        enabled() { return mode !== 'false-enabled'; },
        emit(value) {
          if (!isMainThread && mode === 'worker-exit') process.exit(23);
          if (mode === 'failures' || mode === 'role-counter-width' || mode === 'fallback-failure') throw new TypeError('logger provider failed');
          record('log', value);
          if (!isMainThread && (mode === 'blocked' || mode === 'overflow-width')) barrier(process.env.ZLINK_TEST_TELEMETRY_GATE);
        }
      }; } });
    }
  }
  if (mode === 'trace' || mode === 'blocked-trace' || mode === 'both' || mode === 'failures' || mode === 'role-counter-width' || mode === 'fallback-failure' || mode === 'trace-init-failure') {
    trace.setGlobalTracerProvider({ getTracer() {
      if (mode === 'trace-init-failure') throw new RangeError('trace initialization failed');
      return { startSpan(eventName, options) {
      if (mode === 'failures' || mode === 'role-counter-width' || mode === 'fallback-failure') throw new RangeError('trace provider failed');
      record('span', { eventName, attributes: options.attributes });
      if (!isMainThread && mode === 'blocked-trace') barrier(process.env.ZLINK_TEST_TELEMETRY_GATE);
      return { setStatus() {}, end() {} };
    } }; } });
  }
}

if (!isMainThread && (mode === 'failures' || mode === 'role-counter-width' || mode === 'fallback-failure')) {
  process.stderr.write = function (message) {
    record('fallback', message);
    if (mode === 'fallback-failure') throw new Error('fallback failed');
    return true;
  };
}
