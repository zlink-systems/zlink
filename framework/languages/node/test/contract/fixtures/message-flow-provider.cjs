'use strict';

const { isMainThread, BroadcastChannel } = require('node:worker_threads');
if (!isMainThread && process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL) {
  const { logs } = require('@opentelemetry/api-logs');
  const { trace } = require('@opentelemetry/api');
  const { LoggerProvider } = require('@opentelemetry/sdk-logs');
  const channel = new BroadcastChannel(process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL);
  let failProvider = false;
  logs.setGlobalLoggerProvider(new LoggerProvider({ processors: [{
    onEmit(record) {
      if (failProvider) throw new Error('logger provider failed');
      if (process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR) {
        const fs = require('node:fs'), path = require('node:path');
        fs.mkdirSync(process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR, { recursive: true });
        fs.appendFileSync(path.join(process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR, `${process.pid}.flow.jsonl`),
          JSON.stringify({ pid: process.pid, timestamp: new Date().toISOString(),
            eventName: record.eventName, body: record.body, attributes: record.attributes }) + '\n');
      }
      channel.postMessage({ kind: 'log', record: {
        eventName: record.eventName, timestamp: record.timestamp,
        severityNumber: record.severityNumber, severityText: record.severityText,
        body: record.body, attributes: record.attributes
      } });
    },
    forceFlush() { return Promise.resolve(); },
    shutdown() { return Promise.resolve(); }
  }] }));
  trace.setGlobalTracerProvider({ getTracer() { return { startSpan(eventName, options) {
    channel.postMessage({ kind: 'span', record: { eventName, attributes: options.attributes } });
    return { setStatus() {}, end() {} };
  } }; } });
  const write = process.stderr.write.bind(process.stderr);
  process.stderr.write = message => {
    channel.postMessage({ kind: 'fallback', record: message });
    return write(message);
  };
  channel.onmessage = ({ data }) => {
    if (data.kind === 'fail') failProvider = data.value;
    channel.postMessage({ kind: 'ack', id: data.id });
  };
}
