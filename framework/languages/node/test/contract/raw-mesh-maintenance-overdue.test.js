'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { fixture } = require('./fixtures/raw-mesh-close');

// #5357: an overdue maintenance deadline is scheduled with delay 0 (no negative setTimeout delay,
// which Node 23+ reports as TimeoutNegativeWarning) and runs maintenance on the next timer turn.
test('an overdue maintenance deadline runs on the next timer turn without a negative delay', async () => {
  const f = fixture();
  await f.backend.pumpTask;
  const delays = [];
  const originalSetTimeout = global.setTimeout;
  let expired;
  const ran = new Promise((resolve) => {
    expired = resolve;
  });
  // The backend unrefs its maintenance timer when no operation is pending; keep the loop alive.
  const keepAlive = setInterval(() => {}, 1000);
  let overdue = true;
  f.runtime.nextMaintenanceDeadlineMs = (nowMs) =>
    overdue ? nowMs - 50 : Number.POSITIVE_INFINITY;
  const originalExpire = f.runtime.expireOperations.bind(f.runtime);
  f.runtime.expireOperations = (...args) => {
    overdue = false;
    expired();
    return originalExpire(...args);
  };
  try {
    global.setTimeout = (callback, delay, ...rest) => {
      delays.push(delay);
      return originalSetTimeout(callback, delay, ...rest);
    };
    if (f.backend.maintenanceTimer !== undefined) {
      clearTimeout(f.backend.maintenanceTimer);
      f.backend.maintenanceTimer = undefined;
      f.backend.maintenanceDueMs = Number.POSITIVE_INFINITY;
    }
    f.backend.scheduleMaintenance();
    global.setTimeout = originalSetTimeout;
    assert.deepEqual(delays, [0]);
    await ran;
  } finally {
    global.setTimeout = originalSetTimeout;
    clearInterval(keepAlive);
    await f.backend.close();
  }
});
