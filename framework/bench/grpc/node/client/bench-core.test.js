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

test('completion at the deadline is excluded even before the boundary timer runs', (t) => {
  const originalNow = header.nowNs;
  let now = 90n;
  header.nowNs = () => now;
  t.after(() => {
    header.nowNs = originalNow;
  });
  const metrics = new core.SourceMetrics(100);
  metrics.reset(100n);
  const first = metrics.begin();
  now = 99n;
  metrics.complete(first, true);
  const second = metrics.begin();
  now = 100n;
  metrics.complete(second, true);
  const result = metrics.result();
  assert.equal(result.completed, 2);
  assert.equal(result.completedAtClose, 1);
  assert.equal(result.meanMicros, 0.009);
});

test('terminal snapshot excludes late success and failure at the drain bound', () => {
  const originalNow = header.nowNs;
  let now = 1n;
  header.nowNs = () => now;
  try {
    const metrics = new core.SourceMetrics(10);
    metrics.reset(10n);
    const first = metrics.begin();
    const second = metrics.begin();
    const third = metrics.begin();
    metrics.complete(first, true);
    now = 20n;
    metrics.freezeTerminal();
    now = 21n;
    metrics.complete(second, true);
    metrics.complete(third, false, new Error('late'));
    const result = metrics.result();
    assert.equal(result.completed + result.errors + result.abandoned, metrics.submitted);
    assert.equal(result.completed, 1);
    assert.equal(result.errors, 0);
    assert.deepEqual(result.clientErrorSummary, []);
    assert.equal(result.abandoned, 2);
  } finally {
    header.nowNs = originalNow;
  }
});

test('warmup runs the selected pattern for its duration with warmup headers', async (t) => {
  installTargetStats(t);
  const metrics = new core.SourceMetrics(10);
  let sent = 0;
  const transport = {
    send: async (_stream, payload) => {
      assert.equal(header.decode(payload).phase, header.PHASE_WARMUP);
      sent += 1;
    }
  };
  const warmup = await core.runActive(
    transport,
    metrics,
    { ...options, warmup: 0 },
    activeTrigger('send-saturation', { phase: 'warmup' })
  );
  assert.ok(sent > 0);
  assert.equal(warmup.completed, sent);
  assert.equal(warmup.abandoned, 0);
});
