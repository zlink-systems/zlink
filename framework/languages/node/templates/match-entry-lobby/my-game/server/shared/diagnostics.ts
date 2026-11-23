import { createWriteStream, mkdirSync } from 'node:fs';
import { finished } from 'node:stream/promises';
import { join } from 'node:path';
import { getEnvironmentData, setEnvironmentData } from 'node:worker_threads';
import { logs } from '@opentelemetry/api-logs';
import {
  LoggerProvider,
  SimpleLogRecordProcessor,
  type ReadableLogRecord,
  type LogRecordExporter
} from '@opentelemetry/sdk-logs';
import { LogFiles } from './constants';
import type { NodeConfig } from './config';

type LogConfig = Pick<NodeConfig, 'id' | 'logDir'>;

// Each record goes to stderr (the console) and to `<logDir>/<node-id>.log` as one JSON line.
function registerDiagnostics(config: LogConfig): LoggerProvider {
  const directory = config.logDir ?? LogFiles.directory;
  mkdirSync(directory, { recursive: true });
  // One append stream per thread: writes stay asynchronous and keep their order.
  const file = createWriteStream(join(directory, config.id + LogFiles.nodeExtension), {
    flags: 'a'
  });
  const exporter: LogRecordExporter = {
    export(records: ReadableLogRecord[], callback) {
      for (const record of records) {
        const line =
          JSON.stringify({
            time: new Date().toISOString(),
            body: record.body,
            ...record.attributes
          }) + '\n';
        process.stderr.write(line);
        file.write(line);
      }
      callback({ code: 0 });
    },
    async forceFlush() {},
    async shutdown() {
      file.end();
      await finished(file);
    }
  };
  const diagnostics = new LoggerProvider({
    processors: [new SimpleLogRecordProcessor({ exporter })]
  });
  logs.setGlobalLoggerProvider(diagnostics);
  return diagnostics;
}

// The Framework delivers message flow records only to the provider installed in its telemetry
// worker, which inherits the standard preload settings (message flow tracing spec, Node worker).
const workerConfig = getEnvironmentData(LogFiles.workerConfig) as LogConfig | undefined;
if (workerConfig) registerDiagnostics(workerConfig);

export function startDiagnostics(config: LogConfig): LoggerProvider {
  setEnvironmentData(LogFiles.workerConfig, { id: config.id, logDir: config.logDir });
  const preload = require.resolve('./diagnostics').replace(/\\/g, '/');
  process.env.NODE_OPTIONS = `${process.env.NODE_OPTIONS ?? ''} --require "${preload}"`;
  return registerDiagnostics(config);
}
