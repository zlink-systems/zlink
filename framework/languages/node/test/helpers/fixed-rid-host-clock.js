'use strict';

const { performance } = require('node:perf_hooks');

function fixedRidHostClock(t) {
  let offsetMs = 0;
  const monotonicNow = performance.now.bind(performance);
  const readNow = () => monotonicNow() + offsetMs;
  if (t === undefined) performance.now = readNow;
  else t.mock.method(performance, 'now', readNow);
  return {
    advance(milliseconds) {
      offsetMs += milliseconds;
    }
  };
}

async function finishFixedRidHostStartup(
  host,
  clock,
  preparingState,
  releaseDelayMs = 3,
  maxRecoveries = host.options.registration.spotNodes.size + 1
) {
  const runtime = host.locationOwner.currentRuntime;
  for (let attempt = 0; host.status.state === preparingState && attempt < maxRecoveries; attempt += 1) {
    const recovery = waitForNextHostOwnerRecovery(host);
    clock.advance(releaseDelayMs);
    const renewed = await runtime.renewOwnerLeaseOnce();
    if (!renewed) throw new Error('Fixed RID test host owner lease renewal failed.');
    await recovery;
  }
}

function waitForNextHostOwnerRecovery(host) {
  const original = host.resumeOwnerLeaseOwnedWork.bind(host);
  let resolve;
  const completed = new Promise((done) => {
    resolve = done;
  });
  host.resumeOwnerLeaseOwnedWork = async (...args) => {
    try {
      return await original(...args);
    } finally {
      host.resumeOwnerLeaseOwnedWork = original;
      resolve();
    }
  };
  return completed;
}

module.exports = { fixedRidHostClock, finishFixedRidHostStartup };
