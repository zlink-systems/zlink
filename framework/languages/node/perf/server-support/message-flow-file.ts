import * as fs from 'node:fs';
import { logs } from '@opentelemetry/api-logs';
import { ExportResult, ExportResultCode } from '@opentelemetry/core';
import {
  LoggerProvider,
  LogRecordExporter,
  ReadableLogRecord,
  SimpleLogRecordProcessor
} from '@opentelemetry/sdk-logs';
import { PerfClock } from '../shared/clock';
import { DiagnosticsConfig } from '../shared/contracts';

// Diagnostic runs only (perf §21, message-flow spec 26): the Framework publishes zlink.message_flow and
// zlink.dispatch_error records through the OpenTelemetry logs API; this file exporter keeps every one of them.
class FlowFileExporter implements LogRecordExporter {
  private readonly fd: number;
  constructor(path: string) {
    this.fd = fs.openSync(path, 'wx');
  }

  export(records: ReadableLogRecord[], done: (result: ExportResult) => void): void {
    for (const record of records) {
      fs.writeSync(
        this.fd,
        JSON.stringify({
          eventId: typeof record.body === 'string' ? record.body : String(record.body),
          observedTicks: PerfClock.now().toString(),
          clockDomainId: PerfClock.domain,
          severity: record.severityText,
          tags: record.attributes
        }) + '\n'
      );
    }
    done({ code: ExportResultCode.SUCCESS });
  }

  async forceFlush(): Promise<void> {}
  async shutdown(): Promise<void> {
    fs.closeSync(this.fd);
  }
}

export function enableFlowFileLogging(diagnostics: DiagnosticsConfig): void {
  if (diagnostics.level !== 'Normal')
    throw new Error('Diagnostic runs use Normal message-flow tracing.');
  const provider = new LoggerProvider({
    processors: [
      new SimpleLogRecordProcessor({ exporter: new FlowFileExporter(diagnostics.flowFile) })
    ]
  });
  logs.setGlobalLoggerProvider(provider);
}
