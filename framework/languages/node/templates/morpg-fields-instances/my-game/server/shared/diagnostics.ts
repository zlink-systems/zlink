import { logs } from '@opentelemetry/api-logs';
import {
  LoggerProvider,
  SimpleLogRecordProcessor,
  type ReadableLogRecord,
  type LogRecordExporter
} from '@opentelemetry/sdk-logs';
const exporter: LogRecordExporter = {
  export(records: ReadableLogRecord[], callback) {
    for (const record of records) {
      const eventId = record.attributes.event_id;
      const prefix =
        eventId === 'zlink.message_flow' || eventId === 'zlink.dispatch_error'
          ? 'ZLINK_MESSAGE_FLOW '
          : '';
      process.stderr.write(
        prefix +
          JSON.stringify({
            time: new Date().toISOString(),
            body: record.body,
            ...record.attributes
          }) +
          '\n'
      );
    }
    callback({ code: 0 });
  },
  async forceFlush() {},
  async shutdown() {}
};
export const diagnostics = new LoggerProvider({
  processors: [new SimpleLogRecordProcessor({ exporter })]
});
logs.setGlobalLoggerProvider(diagnostics);
