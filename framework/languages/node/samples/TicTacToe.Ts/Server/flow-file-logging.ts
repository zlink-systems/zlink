import fs from 'node:fs';
import path from 'node:path';
import { logs } from '@opentelemetry/api-logs';
import {
  LoggerProvider,
  SimpleLogRecordProcessor,
  type LogRecordExporter,
  type ReadableLogRecord
} from '@opentelemetry/sdk-logs';
import { ExportResultCode, type ExportResult } from '@opentelemetry/core';

//  The framework's message tracing (spec 46 dispatch observability) publishes
//  zlink.message_flow / zlink.dispatch_error records through the OpenTelemetry
//  logs API. Server processes register this file exporter so the sample run
//  keeps a per-role flow log under the runner's logDir; without a registered
//  provider the tracing API is a no-op and the flow directory stays empty.
class FlowFileExporter implements LogRecordExporter {
  constructor(private readonly filePath: string) {}

  export(records: ReadableLogRecord[], done: (result: ExportResult) => void): void {
    const lines = records
      .map((record) => JSON.stringify({
        ts: new Date().toISOString(),
        event: typeof record.body === 'string' ? record.body : String(record.body),
        severity: record.severityText,
        ...record.attributes
      }))
      .join('\n');
    fs.appendFile(this.filePath, `${lines}\n`, () =>
      done({ code: ExportResultCode.SUCCESS }));
  }

  forceFlush(): Promise<void> {
    return Promise.resolve();
  }

  shutdown(): Promise<void> {
    return Promise.resolve();
  }
}

function enableFlowFileLogging(role: string): void {
  const flagIndex = process.argv.indexOf('--config');
  if (flagIndex < 0 || !Object.hasOwn(process.argv, flagIndex + 1)) return;
  const configPath = process.argv[flagIndex + 1]!;
  try {
    const document = JSON.parse(
      fs.readFileSync(configPath, 'utf8')
    ) as { sample?: { logDir?: string } };
    const logDir = document.sample?.logDir;
    if (logDir === undefined) return;
    fs.mkdirSync(logDir, { recursive: true });
    const provider = new LoggerProvider({
      processors: [
        new SimpleLogRecordProcessor({
          exporter: new FlowFileExporter(path.join(logDir, `flow-${role}.log`))
        })
      ]
    });
    logs.setGlobalLoggerProvider(provider);
  } catch {
    //  Diagnostics wiring only: a missing or malformed config must not stop
    //  the sample server; the run just proceeds without file flow logs.
  }
}

export { enableFlowFileLogging };
