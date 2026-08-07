import { logs } from '@opentelemetry/api-logs';
import { LoggerProvider } from '@opentelemetry/sdk-logs';

export interface E2eTelemetryLogRecord {
  readonly eventId?: string;
  readonly attributes: Readonly<Record<string, unknown>>;
}

let receiveRecord: (record: E2eTelemetryLogRecord) => void = () => undefined;

const provider = new LoggerProvider({
  processors: [{
    onEmit(record) {
      receiveRecord({ eventId: record.eventName, attributes: record.attributes });
    },
    forceFlush: async () => undefined,
    shutdown: async () => undefined
  }]
});
logs.setGlobalLoggerProvider(provider);

export function setE2eTelemetryLogReceiver(
  receiver: (record: E2eTelemetryLogRecord) => void
): void {
  receiveRecord = receiver;
}
