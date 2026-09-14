'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { json } = require('./contracts');
function flowCapture(diagnostics) {
  if (!diagnostics) return null;
  if (diagnostics.level !== 'Normal' || !diagnostics.flowFile) throw new Error('Diagnostic config requires Normal and flowFile.');
  const { logs } = require('@opentelemetry/api-logs');
  const { LoggerProvider } = require('@opentelemetry/sdk-logs');
  fs.mkdirSync(path.dirname(diagnostics.flowFile), { recursive: true });
  fs.writeFileSync(diagnostics.flowFile, '');
  const provider = new LoggerProvider({ processors: [{
    onEmit(record) { fs.appendFileSync(diagnostics.flowFile, `${json({ body: record.body, attributes: record.attributes, timestamp: record.hrTime })}\n`); },
    async forceFlush() {}, async shutdown() {}
  }] });
  logs.setGlobalLoggerProvider(provider);
  return provider;
}
module.exports = { flowCapture };
