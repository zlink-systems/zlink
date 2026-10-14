import { logs } from "@opentelemetry/api-logs";
import {
  LoggerProvider,
  SimpleLogRecordProcessor,
  type ReadableLogRecord,
  type LogRecordExporter,
} from "@opentelemetry/sdk-logs";
const exporter: LogRecordExporter = {
  export(records: ReadableLogRecord[], callback) {
    for (const record of records)
      process.stderr.write(
        JSON.stringify({
          time: new Date().toISOString(),
          body: record.body,
          ...record.attributes,
        }) + "\n",
      );
    callback({ code: 0 });
  },
  async forceFlush() {},
  async shutdown() {},
};
export const diagnostics = new LoggerProvider({
  processors: [new SimpleLogRecordProcessor({ exporter })],
});
logs.setGlobalLoggerProvider(diagnostics);
