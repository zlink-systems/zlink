/* SPDX-License-Identifier: Elastic-2.0 */

const fs = require('node:fs');
const { isMainThread } = require('node:worker_threads');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');

const file = process.env.ZLINK_CROSS_JOIN_FLOW_FILE;
if (!isMainThread && file) {
  const provider = new LoggerProvider({
    processors: [
      {
        onEmit(record) {
          fs.appendFileSync(file, `${record.body}\n`);
        },
        forceFlush: async () => undefined,
        shutdown: async () => undefined
      }
    ]
  });
  logs.setGlobalLoggerProvider(provider);
}
