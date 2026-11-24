import { appendFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { logs } from '@opentelemetry/api-logs';
import { LoggerProvider, SimpleLogRecordProcessor } from '@opentelemetry/sdk-logs';

// The Framework telemetry worker inherits the Node preload and the log path.
export const logPath =
  process.env.CONNECTOR_EXAMPLE_LOG ??
  resolve('.logs', 'run-' + process.pid + '-' + Date.now(), 'server.log');
process.env.CONNECTOR_EXAMPLE_LOG = logPath;
mkdirSync(dirname(logPath), { recursive: true });

export const tracing = new LoggerProvider({
  processors: [
    new SimpleLogRecordProcessor({
      exporter: {
        export(records, callback) {
          for (const record of records) {
            appendFileSync(
              logPath,
              JSON.stringify({ body: record.body, ...record.attributes }) + '\n'
            );
          }
          callback({ code: 0 });
        },
        async forceFlush() {},
        async shutdown() {}
      }
    })
  ]
});
logs.setGlobalLoggerProvider(tracing);
