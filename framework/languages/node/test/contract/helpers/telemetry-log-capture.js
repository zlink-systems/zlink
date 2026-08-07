'use strict';

const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');

const records = [];
const provider = new LoggerProvider({
  processors: [{
    onEmit(record) {
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
