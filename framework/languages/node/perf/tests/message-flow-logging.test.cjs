const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createRequire } = require('node:module');
const test = require('node:test');

test('Framework logger created before perf provider registration exports alongside a later logger', async () => {
  const frameworkRequire = createRequire(require.resolve('@zlink-systems/framework'));
  const frameworkLogs = frameworkRequire('@opentelemetry/api-logs').logs;
  const perfLogs = require('@opentelemetry/api-logs').logs;
  const beforeProvider = frameworkLogs.getLogger('perf-before-provider');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-perf-flow-'));
  const flowFile = path.join(directory, 'flow.jsonl');
  try {
    const { enableFlowFileLogging } = require('../build/server-support/message-flow-file.js');
    enableFlowFileLogging({ level: 'Normal', flowFile });
    beforeProvider.emit({ body: 'before-provider' });
    perfLogs.getLogger('perf-after-provider').emit({ body: 'after-provider' });
    // The positive control proves that the exporter works even when the earlier proxy stays disconnected.
    const records = fs
      .readFileSync(flowFile, 'utf8')
      .trim()
      .split('\n')
      .filter(Boolean)
      .map(JSON.parse);
    assert.equal(records.filter((record) => record.eventId === 'after-provider').length, 1);
    assert.equal(records.filter((record) => record.eventId === 'before-provider').length, 1);
    assert.equal(frameworkLogs, perfLogs);
  } finally {
    await perfLogs.getLoggerProvider().shutdown();
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
