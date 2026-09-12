// SPDX-License-Identifier: MPL-2.0
'use strict';

const os = require('node:os');
const { performance } = require('node:perf_hooks');
const header = require('../shared/bench-metric-header');

const LOGICAL_CORES = os.cpus().length;
const CLIENT_SATURATION_METRIC = 'event_loop_utilization';
const CLIENT_PARALLELISM_CEILING = 1.0;
const ERROR_KIND_LIMIT = 8;
const ERROR_MESSAGE_LIMIT = 200;

class ResourceSample {
  constructor() {
    this.cpuStart = process.cpuUsage();
    this.eluStart = performance.eventLoopUtilization();
    this.startNs = header.nowNs();
  }

  finish() {
    const cpu = process.cpuUsage(this.cpuStart);
    const elu = performance.eventLoopUtilization(this.eluStart);
    const elapsedSeconds = Number(header.nowNs() - this.startNs) / 1e9;
    const cpuSeconds = (cpu.user + cpu.system) / 1e6;
    const cores = elapsedSeconds > 0 ? cpuSeconds / elapsedSeconds : 0;
    return {
      cpuSeconds,
      elapsedSeconds,
      cores,
      cpuPercent: (cores / LOGICAL_CORES) * 100,
      eventLoopUtilization: elu.utilization,
      memoryMb: process.memoryUsage().rss / 1024 / 1024
    };
  }
}

class SourceMetrics {
  constructor(sampleLimit) {
    this.sampleLimit = sampleLimit;
    this.reset();
  }

  reset() {
    this.submitted = 0;
    this.completed = 0;
    this.errors = 0;
    this.inFlight = 0;
    this.peakInFlight = 0;
    this.abandoned = 0;
    this.sampleCount = 0;
    this.sampleSumMicros = 0;
    this.samples = [];
    this.errorSummary = new Map();
    this.otherErrors = 0;
  }

  begin() {
    this.submitted += 1;
    this.inFlight += 1;
    this.peakInFlight = Math.max(this.peakInFlight, this.inFlight);
    return header.nowNs();
  }

  complete(started, success, error = null) {
    const micros = Number(header.nowNs() - started) / 1000;
    this.inFlight -= 1;
    if (success) this.completed += 1;
    else {
      this.errors += 1;
      if (error !== null) this.recordError(error);
    }
    this.sampleCount += 1;
    this.sampleSumMicros += micros;
    if (this.samples.length < this.sampleLimit) this.samples.push(micros);
  }

  recordError(error) {
    const type = error && error.name ? String(error.name) : typeof error;
    let message = error && error.message !== undefined ? String(error.message) : String(error);
    message = message.replace(/[\r\n]/g, ' ').slice(0, ERROR_MESSAGE_LIMIT);
    const key = JSON.stringify([type, message]);
    const existing = this.errorSummary.get(key);
    if (existing !== undefined) {
      existing.count += 1;
    } else if (this.errorSummary.size < ERROR_KIND_LIMIT) {
      this.errorSummary.set(key, { type, message, count: 1 });
    } else {
      this.otherErrors += 1;
    }
  }

  recordAbandoned(count) {
    this.abandoned = Math.max(this.abandoned, count);
  }

  snapshot() {
    return {
      submitted: this.submitted,
      completed: this.completed,
      errors: this.errors,
      received: 0,
      inFlight: this.inFlight,
      peakInFlight: this.peakInFlight
    };
  }

  result() {
    const sorted = this.samples.slice().sort((a, b) => a - b);
    return {
      completed: this.completed,
      errors: this.errors,
      inFlight: this.inFlight,
      peakInFlight: this.peakInFlight,
      abandoned: this.abandoned,
      clientErrorSummary: [...this.errorSummary.values()],
      clientErrorOtherCount: this.otherErrors,
      meanMicros: this.sampleCount === 0 ? 0 : this.sampleSumMicros / this.sampleCount,
      p95Micros: percentile(sorted, 0.95),
      p99Micros: percentile(sorted, 0.99)
    };
  }
}

function percentile(sorted, fraction) {
  if (sorted.length === 0) return 0;
  const index = Math.ceil(fraction * sorted.length) - 1;
  return sorted[Math.min(Math.max(index, 0), sorted.length - 1)];
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function sleepImmediate() {
  return new Promise((resolve) => setImmediate(resolve));
}

function pollTimeoutUntil(deadline, maxWaitMs) {
  const remainingNs = deadline - header.nowNs();
  if (remainingNs <= 0n) return 0;
  const remainingMs = Number(remainingNs / 1_000_000n);
  return Math.min(maxWaitMs, Math.max(1, remainingMs));
}

async function resetTarget(statsUrl) {
  const response = await fetch(`${statsUrl}/bench/reset`, { method: 'POST' });
  if (!response.ok) throw new Error(`target reset failed: ${response.status}`);
}

async function targetStats(statsUrl) {
  const response = await fetch(`${statsUrl}/bench/stats`);
  if (!response.ok) throw new Error(`target stats failed: ${response.status}`);
  return response.json();
}

async function waitForRouteReady(probe, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let last = null;
  while (Date.now() < deadline) {
    try {
      await probe();
      return;
    } catch (error) {
      last = error;
      await delay(20);
    }
  }
  throw new Error(`target route did not become ready within ${timeoutMs}ms: ${last && last.message}`);
}

function headerRunId(runId) {
  let hash = 2166136261;
  for (const value of Buffer.from(runId, 'utf8')) {
    hash = Math.imul(hash ^ value, 16777619) >>> 0;
  }
  return hash === 0 ? 1 : hash;
}

async function runWarmup(transport, options, trigger) {
  const runId = headerRunId(trigger.runId);
  for (let index = 0; index < options.warmup; index++) {
    const stream = trigger.pattern === 'send-saturation'
      ? index % trigger.sendConcurrency
      : 0;
    const payload = header.createPayloadBytes(
      trigger.payloadBytes, runId, header.PHASE_WARMUP, index
    );
    if (trigger.pattern === 'send-saturation') await transport.send(stream, payload);
    else {
      const reply = await transport.request(stream, payload);
      validateReply(reply, runId, header.PHASE_WARMUP, trigger.payloadBytes, index);
    }
  }
}

async function runActive(transport, metrics, options, trigger) {
  await resetTarget(options.targetStatsUrl);
  metrics.reset();
  const resources = new ResourceSample();
  const deadline = header.nowNs() + BigInt(Math.round(trigger.durationMs * 1e6));
  const runId = headerRunId(trigger.runId);
  let sequence = 0;
  const nextSequence = () => sequence++;

  if (trigger.pattern === 'request-serial') {
    await requestWorkers(1, transport, metrics, trigger, runId, nextSequence, deadline);
  } else if (trigger.pattern === 'request-window') {
    await requestWorkers(
      trigger.requestWindow, transport, metrics, trigger, runId, nextSequence, deadline
    );
  } else if (trigger.pattern === 'request-backpressure') {
    await requestBackpressure(
      transport, metrics, options, trigger, runId, nextSequence, deadline
    );
  } else if (trigger.pattern === 'send-saturation') {
    await sendWorkers(
      trigger.sendConcurrency, transport, metrics, trigger, runId, nextSequence, deadline
    );
  } else {
    throw new Error(`unsupported pattern ${trigger.pattern}`);
  }

  const usage = resources.finish();
  const target = await targetStats(options.targetStatsUrl);
  const source = metrics.result();
  const send = trigger.pattern === 'send-saturation';
  const completed = send ? target.received : source.completed;
  const seconds = trigger.durationMs / 1000;
  const meanMicros = send ? target.meanMicros : source.meanMicros;
  const p95Micros = send ? target.p95Micros : source.p95Micros;
  const p99Micros = send ? target.p99Micros : source.p99Micros;
  const throughput = completed / Math.max(0.001, seconds);
  return {
    completed: source.completed,
    errors: source.errors,
    client_error_summary: source.clientErrorSummary,
    client_error_other_count: source.clientErrorOtherCount,
    server_errors: target.errors,
    throughput_per_second: throughput,
    bandwidth_mb_s: throughput * trigger.payloadBytes / 1_000_000,
    latency_mean_ms: meanMicros / 1000,
    latency_p95_ms: p95Micros / 1000,
    latency_p99_ms: p99Micros / 1000,
    client_cpu_percent: usage.cpuPercent,
    client_memory_mb: usage.memoryMb,
    server_cpu_percent: target.cpuSeconds / Math.max(0.001, seconds) / LOGICAL_CORES * 100,
    server_memory_mb: target.workingSetMb,
    client_cores: usage.cores,
    client_parallelism_ceiling: CLIENT_PARALLELISM_CEILING,
    client_saturation_metric: CLIENT_SATURATION_METRIC,
    event_loop_utilization: usage.eventLoopUtilization,
    peak_in_flight: source.peakInFlight,
    request_window: trigger.pattern === 'request-window' ? trigger.requestWindow : null,
    abandoned: source.abandoned,
    server_received_at_close: send ? target.received : null
  };
}

async function requestWorkers(count, transport, metrics, trigger, runId, nextSequence, deadline) {
  const workers = Array.from({ length: count }, () => (async () => {
    while (header.nowNs() < deadline) {
      await executeRequest(transport, metrics, trigger, runId, 0, nextSequence());
    }
  })());
  await Promise.all(workers);
}

async function requestBackpressure(
  transport, metrics, options, trigger, runId, nextSequence, deadline
) {
  if (typeof transport.requestSubmission === 'function') {
    const pending = new Set();
    const completionPump = typeof transport.openRequestCompletionPump === 'function'
      ? transport.openRequestCompletionPump()
      : null;
    let blocked = null;
    try {
      while (header.nowNs() < deadline) {
        // A backpressured socket skips this sweep only. Its admission task is
        // observed separately so the completion pump and other event-loop work
        // continue to make progress.
        if (blocked === null) {
          const sequence = nextSequence();
          const payload = header.createPayloadBytes(
            trigger.payloadBytes, runId, header.PHASE_ACTIVE, sequence
          );
          const started = metrics.begin();
          let submission;
          try {
            submission = transport.requestSubmission(0, payload);
          } catch (error) {
            metrics.complete(started, false, error);
          }
          if (submission !== undefined) {
            const reply = (async () => {
              try {
                const value = await submission.reply;
                validateReply(value, runId, header.PHASE_ACTIVE, trigger.payloadBytes, sequence);
                metrics.complete(started, true);
              } catch (error) {
                metrics.complete(started, false, error);
              }
            })();
            pending.add(reply);
            reply.finally(() => pending.delete(reply));
            if (submission.result === transport.backpressuredResult) {
              blocked = submission.admitted.catch(() => {});
              blocked.finally(() => { blocked = null; });
            }
          }
        }
        completionPump?.poll(blocked === null ? 0 : pollTimeoutUntil(deadline, 50));
        await sleepImmediate();
      }

      if (completionPump !== null) {
        const drainDeadline = header.nowNs() + BigInt(options.drainBoundMs) * 1_000_000n;
        while ((pending.size > 0 || blocked !== null) && header.nowNs() < drainDeadline) {
          completionPump.poll(pollTimeoutUntil(drainDeadline, 50));
          await sleepImmediate();
        }
        if (pending.size > 0 || blocked !== null) metrics.recordAbandoned(metrics.inFlight);
        return;
      }

      if (pending.size === 0) return;
      let drained = false;
      await Promise.race([
        Promise.all([...pending]).then(() => { drained = true; }),
        delay(options.drainBoundMs)
      ]);
      if (!drained) metrics.recordAbandoned(metrics.inFlight);
    } finally {
      completionPump?.close();
    }
  }

  const pending = new Set();
  let issuedSinceYield = 0;
  while (header.nowNs() < deadline) {
    const operation = executeRequest(transport, metrics, trigger, runId, 0, nextSequence());
    pending.add(operation);
    operation.finally(() => pending.delete(operation));
    issuedSinceYield += 1;
    if (issuedSinceYield === 256) {
      issuedSinceYield = 0;
      await new Promise((resolve) => setImmediate(resolve));
    }
  }
  if (pending.size === 0) return;
  let drained = false;
  await Promise.race([
    Promise.all([...pending]).then(() => { drained = true; }),
    delay(options.drainBoundMs)
  ]);
  if (!drained) metrics.recordAbandoned(metrics.inFlight);
}

async function sendWorkers(count, transport, metrics, trigger, runId, nextSequence, deadline) {
  const workers = Array.from({ length: count }, (_, stream) => (async () => {
    while (header.nowNs() < deadline) {
      const sequence = nextSequence();
      const payload = header.createPayloadBytes(
        trigger.payloadBytes, runId, header.PHASE_ACTIVE, sequence
      );
      const started = metrics.begin();
      try {
        if (typeof transport.sendSubmission === 'function') {
          const submission = transport.sendSubmission(stream, payload);
          if (submission.result === transport.backpressuredResult) {
            await submission.admitted;
          }
        } else {
          await transport.send(stream, payload);
        }
        metrics.complete(started, true);
      } catch (error) {
        metrics.complete(started, false, error);
      }
      // Keep all logical send streams and completion callbacks runnable when
      // a normal submission does not otherwise await anything.
      await sleepImmediate();
    }
  })());
  await Promise.all(workers);
}

async function executeRequest(transport, metrics, trigger, runId, stream, sequence) {
  const payload = header.createPayloadBytes(
    trigger.payloadBytes, runId, header.PHASE_ACTIVE, sequence
  );
  const started = metrics.begin();
  try {
    const reply = await transport.request(stream, payload);
    validateReply(reply, runId, header.PHASE_ACTIVE, trigger.payloadBytes, sequence);
    metrics.complete(started, true);
  } catch (error) {
    metrics.complete(started, false, error);
  }
}

function validateReply(reply, runId, phase, payloadBytes, sequence) {
  const decoded = header.decode(reply);
  if (!header.isExpected(decoded, runId, phase, payloadBytes, sequence)) {
    throw new Error('echo reply did not carry the expected metric header');
  }
}

function streamDescription(pattern, requestWindow, sendConcurrency) {
  if (pattern === 'request-serial') {
    return { count: 1, inFlightPerStream: 1, implementation: 'Node Promise; one sequential loop' };
  }
  if (pattern === 'request-window') {
    return {
      count: 1,
      inFlightPerStream: requestWindow,
      implementation: `Node event loop; ${requestWindow} Promises share one logical-stream window`
    };
  }
  if (pattern === 'request-backpressure') {
    return {
      count: 1,
      inFlightPerStream: null,
      implementation: 'Node event loop; reply stages separate; pause only on BACKPRESSURED admission'
    };
  }
  if (pattern === 'send-saturation') {
    return {
      count: sendConcurrency,
      inFlightPerStream: 1,
      implementation: 'Node loop per logical stream; pause only on BACKPRESSURED admission'
    };
  }
  throw new Error(`unknown pattern ${pattern}`);
}

module.exports = {
  LOGICAL_CORES,
  CLIENT_SATURATION_METRIC,
  CLIENT_PARALLELISM_CEILING,
  SourceMetrics,
  delay,
  sleepImmediate,
  pollTimeoutUntil,
  waitForRouteReady,
  headerRunId,
  runWarmup,
  runActive,
  streamDescription
};
