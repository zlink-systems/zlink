'use strict';

const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');
const fs = require('node:fs');
const path = require('node:path');
const flowDirectory = process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR;
const flowFile = flowDirectory === undefined ? undefined
  : path.join(flowDirectory, `${process.pid}.flow.jsonl`);
if (flowDirectory !== undefined) fs.mkdirSync(flowDirectory, { recursive: true });

const records = [];
function captureRecord(record, writeFile) {
      if (writeFile && flowFile !== undefined) {
        fs.appendFileSync(flowFile, JSON.stringify({
          pid: process.pid, timestamp: new Date().toISOString(),
          eventName: record.eventName, body: record.body, attributes: record.attributes
        }) + '\n');
      }
      const normalized = {
        eventId: record.eventName,
        severityNumber: record.severityNumber,
        severityText: record.severityText
      };
      for (const [name, value] of Object.entries(record.attributes)) {
        normalized[name.replace(/_([a-z])/g, (_match, letter) => letter.toUpperCase())] = value;
      }
      records.push(normalized);
}
const provider = new LoggerProvider({ processors: [{
  onEmit(record) { captureRecord(record, true); },
  forceFlush() { return Promise.resolve(); },
  shutdown() { return Promise.resolve(); }
}] });
logs.setGlobalLoggerProvider(provider);

const deliveryPath = process.env.ZLINK_TEST_TELEMETRY_MODULE_DIR
  ? path.join(process.env.ZLINK_TEST_TELEMETRY_MODULE_DIR, 'message-flow-delivery.js')
  : '../../../packages/framework/dist/runtime/diagnostics/message-flow-delivery';
const observation = require('../fixtures/message-flow-observation.cjs')(require(deliveryPath), {
  onLog(record) { captureRecord(record, false); }
});

module.exports = {
  observed: observation.observed,
  close: observation.close,
  records,
  reset() { records.length = 0; }
};
