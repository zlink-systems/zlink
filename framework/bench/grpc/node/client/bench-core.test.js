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
  t.after(() => {
    global.fetch = originalFetch;
  });
}

const options = {
  targetStatsUrl: 'http://127.0.0.1:1',
  drainBoundMs: 100
};

test('a reply drained after active close is excluded from rate and latency', async (t) => {
  installTargetStats(t);
  const originalCpuUsage = process.cpuUsage;
  let cpuMicros = 0;
  process.cpuUsage = (start) => ({ user: cpuMicros - (start?.user ?? 0), system: 0 });
  t.after(() => {
    process.cpuUsage = originalCpuUsage;
  });
  const metrics = new core.SourceMetrics(100);
  const polling = setInterval(() => metrics.snapshot(), 1);
  t.after(() => clearInterval(polling));
  const transport = {
    request: async (_stream, payload) => {
      await core.delay(100);
      cpuMicros = 1_000_000;
      return payload;
    }
  };
  const result = await core.runActive(transport, metrics, options, activeTrigger('request-serial'));
  assert.equal(result.completed, 1);
  assert.equal(result.throughput_per_second, 0);
  assert.equal(result.latency_mean_ms, 0);
  assert.equal(result.client_cpu_percent, 0);
  assert.equal(result.completed_at_close, 0);
});

test('send rate and latency use the target snapshot before worker drain', async (t) => {
  const originalFetch = global.fetch;
  let received = 3;
  global.fetch = async () => ({
    ok: true,
    json: async () => ({
      received,
      errors: 0,
      meanMicros: received,
      p95Micros: received,
      p99Micros: received,
      cpuSeconds: 0,
      workingSetMb: 0
    })
  });
  t.after(() => {
    global.fetch = originalFetch;
  });
  const metrics = new core.SourceMetrics(100);
  const polling = setInterval(() => metrics.snapshot(), 1);
  t.after(() => clearInterval(polling));
  const transport = {
    send: async () => {
      await core.delay(100);
      received = 9;
    }
  };
  const result = await core.runActive(
    transport,
    metrics,
    options,
    activeTrigger('send-saturation')
  );
  assert.equal(result.server_received_at_close, 3);
  assert.equal(result.throughput_per_second, 150);
  assert.equal(result.latency_mean_ms, 0.003);
  assert.equal(result.completed, 2);
});

test('request-backpressure pumps completions and observes admission without awaiting it', async (t) => {
  installTargetStats(t);
  const replies = [];
  let resolveAdmission;
  let closed = 0;
  const pollTimeouts = [];
  let submissions = 0;
  const transport = {
    backpressuredResult: 'backpressured',
    requestCompletionPump: {
      poll(timeoutMs) {
        pollTimeouts.push(timeoutMs);
        for (const resolve of replies.splice(0)) resolve();
        resolveAdmission?.();
      },
      close() {
        closed += 1;
      }
    },
    requestSubmission(_stream, payload) {
      submissions += 1;
      let resolveReply;
      const reply = new Promise((resolve) => {
        resolveReply = () => resolve(payload);
      });
      replies.push(resolveReply);
      if (submissions === 1) {
        const admitted = new Promise((resolve) => {
          resolveAdmission = resolve;
        });
        return { result: 'backpressured', admitted, reply };
      }
      return { result: 'accepted', admitted: Promise.resolve(), reply };
    }
  };

  const metrics = new core.SourceMetrics(100);
  const result = await core.runActive(
    transport,
    metrics,
    options,
    activeTrigger('request-backpressure')
  );

  assert.equal(closed, 0, 'the active phase does not close the transport-owned poller');
  assert.ok(submissions > 1, 'submission resumed after the separately observed admission');
  assert.ok(
    pollTimeouts.some((timeoutMs) => timeoutMs > 0),
    'blocked turn waited for completion'
  );
  assert.ok(pollTimeouts.length >= submissions, 'every submit turn advanced the completion pump');
  assert.equal(result.abandoned, 0);
});

test('send-saturation yields so every logical stream starts work', async (t) => {
  installTargetStats(t);
  const streams = new Set();
  const transport = {
    backpressuredResult: 'backpressured',
    awaitSendCompletion: (admitted) => admitted,
    sendSubmission(stream) {
      streams.add(stream);
      return { result: 'accepted', admitted: Promise.resolve() };
    }
  };

  const metrics = new core.SourceMetrics(100);
  await core.runActive(transport, metrics, options, activeTrigger('send-saturation'));

  assert.deepEqual([...streams].sort(), [0, 1]);
});
