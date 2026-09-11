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
const provider = new LoggerProvider({
  processors: [{
    onEmit(record) {
      if (flowFile !== undefined) {
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
    },
    forceFlush() { return Promise.resolve(); },
    shutdown() { return Promise.resolve(); }
  }]
});
logs.setGlobalLoggerProvider(provider);

module.exports = {
  records,
  reset() { records.length = 0; }
};
