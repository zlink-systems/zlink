import fs from 'node:fs';
import path from 'node:path';
import { logs } from '@opentelemetry/api-logs';
import { LoggerProvider } from '@opentelemetry/sdk-logs';

export function configureTelemetryLogProvider(logDir: string, rid: string): void {
  fs.mkdirSync(logDir, { recursive: true });
  const file = path.join(logDir, `${rid}-flow.log`);
  const provider = new LoggerProvider({
    processors: [{
      onEmit(record) {
        fs.appendFileSync(file, `${JSON.stringify({
          eventId: record.eventName,
          severityNumber: record.severityNumber,
          severityText: record.severityText,
          ...record.attributes
        })}\n`, 'utf8');
      },
      forceFlush: async () => undefined,
      shutdown: async () => undefined
    }]
  });
  logs.setGlobalLoggerProvider(provider);
}
