// SPDX-License-Identifier: MPL-2.0
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const core = require('./bench-core');
const header = require('../shared/bench-metric-header');

function activeTrigger(pattern, overrides = {}) {
  return {
    runId: 'bench-core-test',
    pattern,
    payloadBytes: 64,
    durationMs: 20,
    requestWindow: 100,
    sendConcurrency: 2,
    ...overrides
  };
}

function installTargetStats(t, received = 0) {
  const originalFetch = global.fetch;
  global.fetch = async () => ({
    ok: true,
    json: async () => ({
      received,
      errors: 0,
      meanMicros: 0,
      p95Micros: 0,
      p99Micros: 0,
      cpuSeconds: 0,
      workingSetMb: 0
    })
  });
  t.after(() => { global.fetch = originalFetch; });
}

const options = {
  targetStatsUrl: 'http://127.0.0.1:1',
  drainBoundMs: 100
};

test('request-backpressure pumps completions and observes admission without awaiting it', async (t) => {
  installTargetStats(t);
  const replies = [];
  let resolveAdmission;
  let opened = 0;
  let closed = 0;
  const pollTimeouts = [];
  let submissions = 0;
  const transport = {
    backpressuredResult: 'backpressured',
    openRequestCompletionPump() {
      opened += 1;
      return {
        poll(timeoutMs) {
          pollTimeouts.push(timeoutMs);
          for (const resolve of replies.splice(0)) resolve();
          resolveAdmission?.();
        },
        close() { closed += 1; }
      };
    },
    requestSubmission(_stream, payload) {
      submissions += 1;
      let resolveReply;
      const reply = new Promise((resolve) => { resolveReply = () => resolve(payload); });
      replies.push(resolveReply);
      if (submissions === 1) {
        const admitted = new Promise((resolve) => { resolveAdmission = resolve; });
        return { result: 'backpressured', admitted, reply };
      }
      return { result: 'accepted', admitted: Promise.resolve(), reply };
    }
  };

  const metrics = new core.SourceMetrics(100);
  const result = await core.runActive(transport, metrics, options, activeTrigger('request-backpressure'));

  assert.equal(opened, 1);
  assert.equal(closed, 1);
  assert.ok(submissions > 1, 'submission resumed after the separately observed admission');
  assert.ok(pollTimeouts.some((timeoutMs) => timeoutMs > 0), 'blocked turn waited for completion');
  assert.ok(pollTimeouts.length >= submissions, 'every submit turn advanced the completion pump');
  assert.equal(result.abandoned, 0);
});

test('send-saturation yields so every logical stream starts work', async (t) => {
  installTargetStats(t);
  const streams = new Set();
  const transport = {
    backpressuredResult: 'backpressured',
    sendSubmission(stream) {
      streams.add(stream);
      return { result: 'accepted', admitted: Promise.resolve() };
    }
  };

  const metrics = new core.SourceMetrics(100);
  await core.runActive(transport, metrics, options, activeTrigger('send-saturation'));

  assert.deepEqual([...streams].sort(), [0, 1]);
});
