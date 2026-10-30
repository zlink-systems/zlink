'use strict';
require('./telemetry-worker-preload.cjs');
const { appendFileSync } = require('node:fs');
const { isMainThread } = require('node:worker_threads');
module.exports = tag => appendFileSync(process.env.ZLINK_TEST_TELEMETRY_RECORDS,
  JSON.stringify({ kind: 'order', thread: isMainThread ? 'main' : 'worker', value: tag }) + '\n');
