// SPDX-License-Identifier: MPL-2.0
'use strict';

// Pattern drivers shared by every implementation in the node row.
//
// Each driver returns a cell record in the `with-grpc-cell-v1` shape the shared
// aggregator reads (FB-021). Nothing here decides a verdict: medians, G5 and the
// section 7.2 ratios belong to framework/bench/tools, never to a language harness.

const header = require('../shared/bench-metric-header');

const LOGICAL_CORES = require('node:os').cpus().length;

/**
 * spec section 5.1 / FB-019: saturation is judged against the parallelism ceiling the
 * harness DECLARES, not against a share of every logical core. The node client
 * is one JS thread and never starts a worker, so the ceiling is 1. A cell that
 * reaches 0.95 of it measured the client runtime rather than the transport, and
 * the aggregator drops it from throughput comparisons -- that is the correct
 * outcome for this client, not a defect to engineer around by adding threads,
 * which would measure a different client.
 */
const CLIENT_PARALLELISM_CEILING = 1;

class ResourceSample {
  constructor() {
    this.cpuStart = process.cpuUsage();
    this.startNs = header.nowNs();
  }

  finish() {
    const cpu = process.cpuUsage(this.cpuStart);
    const elapsedNs = header.nowNs() - this.startNs;
    const elapsedSeconds = Number(elapsedNs) / 1e9;
    const cpuSeconds = (cpu.user + cpu.system) / 1e6;
    const cores = elapsedSeconds > 0 ? cpuSeconds / elapsedSeconds : 0;
    return {
      cpuSeconds,
      elapsedSeconds,
      cores,
      cpuPercent: (cores / LOGICAL_CORES) * 100,
      memoryMb: process.memoryUsage().rss / 1024 / 1024
    };
  }
}

class Latencies {
  constructor(limit) {
    this.limit = limit;
    this.samples = [];
  }

  add(micros) {
    if (this.samples.length < this.limit) this.samples.push(micros);
  }

  summary() {
    const sorted = this.samples.slice().sort((a, b) => a - b);
    return {
      mean: mean(sorted),
      p95: percentile(sorted, 0.95),
      p99: percentile(sorted, 0.99)
    };
  }
}

function mean(sorted) {
  if (sorted.length === 0) return 0;
  let total = 0;
  for (const value of sorted) total += value;
  return total / sorted.length;
}

function percentile(sorted, fraction) {
  if (sorted.length === 0) return 0;
  const index = Math.ceil(fraction * sorted.length) - 1;
  return sorted[Math.min(Math.max(index, 0), sorted.length - 1)];
}

function elapsedMicros(startNs) {
  return Number(header.nowNs() - startNs) / 1000;
}

async function resetServer(statsUrl) {
  const response = await fetch(`${statsUrl}/bench/reset`, { method: 'POST' });
  if (!response.ok) throw new Error(`reset ${statsUrl} failed: ${response.status}`);
}

async function serverStats(statsUrl) {
  const response = await fetch(`${statsUrl}/bench/stats`);
  if (!response.ok) throw new Error(`stats ${statsUrl} failed: ${response.status}`);
  return response.json();
}

/**
 * spec section 3 / FB-008 settle: no fixed sleep. Poll the server's received count
 * until it stops moving, bounded. On bound expiry the caller marks the next cell
 * that uses this same server contaminated and excludes it, rather than measuring
 * a cell that is standing behind the previous cell's backlog.
 */
async function waitForServerDrain(statsUrl, quietMs, boundMs) {
  const startNs = header.nowNs();
  let latest = null;
  let lastCount = -1;
  let lastChangeMs = 0;
  for (;;) {
    const elapsed = Number(header.nowNs() - startNs) / 1e6;
    if (elapsed >= boundMs) {
      return { snapshot: latest, drainMs: elapsed, boundHit: true };
    }
    latest = await serverStats(statsUrl);
    const count = latest.activeMessages + latest.errors;
    if (count !== lastCount) {
      lastCount = count;
      lastChangeMs = Number(header.nowNs() - startNs) / 1e6;
    } else if (Number(header.nowNs() - startNs) / 1e6 - lastChangeMs >= quietMs) {
      return { snapshot: latest, drainMs: Number(header.nowNs() - startNs) / 1e6, boundHit: false };
    }
    await delay(10);
  }
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/**
 * Bounded route-readiness probe. A ROUTER addressing a peer by routing id fails
 * until that peer's id is in the local routing map, so every socket is probed
 * ONCE, before warmup. It never runs inside a measured window and it is not a
 * retry: the reset that opens the active phase happens after it returns.
 */
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
  throw new Error(`route not ready within ${timeoutMs}ms: ${last && last.message}`);
}

/** `request-serial`: one outstanding request, next one submitted after the reply. */
async function runRequestSerial({ payloadSize, options, statsUrl, operation }) {
  for (let i = 0; i < options.warmup; i++) {
    await operation(payloadSize, header.PHASE_WARMUP, i);
  }
  await resetServer(statsUrl);

  const latencies = new Latencies(options.latencySampleLimit);
  let completed = 0;
  let errors = 0;
  let sequence = 0;
  const resources = new ResourceSample();
  const startNs = header.nowNs();
  const activeUntilNs = startNs + BigInt(Math.round(options.durationSeconds * 1e9));

  while (header.nowNs() < activeUntilNs) {
    const index = sequence++;
    const t0 = header.nowNs();
    try {
      await operation(payloadSize, header.PHASE_ACTIVE, index);
      completed += 1;
    } catch (error) {
      errors += 1;
    }
    latencies.add(elapsedMicros(t0));
  }

  const usage = resources.finish();
  const server = await serverStats(statsUrl);
  return finishRequestCell({
    payloadSize, options, usage, server, latencies, completed, errors,
    peakInFlight: 1, abandoned: 0
  });
}

/**
 * `request-window`: up to `request_window` outstanding requests at a time.
 *
 * Admission is a counter, not a scan over a pending list: FB-010 showed an
 * O(window) drain scan capping the submit rate so hard that the .NET raw row
 * held 8 requests against a configured window of 100. `peak_in_flight` and the
 * abandoned count are reported per cell (FB-017) because that pair is what
 * separates "the harness cannot fill the window" from "the stack only reaches
 * this depth"; without it a wrong premise survives.
 */
async function runRequestWindow({ payloadSize, options, statsUrl, operation }) {
  for (let i = 0; i < options.warmup; i++) {
    await operation(payloadSize, header.PHASE_WARMUP, i);
  }
  await resetServer(statsUrl);

  const latencies = new Latencies(options.latencySampleLimit);
  let inFlight = 0;
  let peakInFlight = 0;
  let completed = 0;
  let errors = 0;
  let sequence = 0;
  let wakeSlot = null;

  const releaseSlot = () => {
    inFlight -= 1;
    if (wakeSlot !== null) {
      const resume = wakeSlot;
      wakeSlot = null;
      resume();
    }
  };

  const resources = new ResourceSample();
  const startNs = header.nowNs();
  const activeUntilNs = startNs + BigInt(Math.round(options.durationSeconds * 1e9));

  while (header.nowNs() < activeUntilNs) {
    if (inFlight >= options.requestWindow) {
      await new Promise((resolve) => { wakeSlot = resolve; });
      continue;
    }
    inFlight += 1;
    if (inFlight > peakInFlight) peakInFlight = inFlight;
    const index = sequence++;
    const t0 = header.nowNs();
    operation(payloadSize, header.PHASE_ACTIVE, index).then(
      () => { completed += 1; latencies.add(elapsedMicros(t0)); },
      () => { errors += 1; latencies.add(elapsedMicros(t0)); }
    ).then(releaseSlot, releaseSlot);
  }

  // Settle the requests already in flight. Bounded; whatever is still
  // outstanding at the bound is counted as abandoned rather than dropped.
  const settleDeadline = Date.now() + options.windowSettleMs;
  while (inFlight > 0 && Date.now() < settleDeadline) {
    await delay(1);
  }
  const abandoned = inFlight;
  if (abandoned > 0) errors += abandoned;

  const usage = resources.finish();
  const server = await serverStats(statsUrl);
  return finishRequestCell({
    payloadSize, options, usage, server, latencies, completed, errors,
    peakInFlight, abandoned
  });
}

function finishRequestCell({
  payloadSize, options, usage, server, latencies, completed, errors, peakInFlight, abandoned
}) {
  const seconds = Math.max(1e-9, options.durationSeconds);
  const throughput = completed / seconds;
  const summary = latencies.summary();
  return {
    throughput_per_second: throughput,
    bandwidth_mb_s: (throughput * payloadSize) / 1e6,
    latency_mean_ms: summary.mean / 1000,
    latency_p95_ms: summary.p95 / 1000,
    latency_p99_ms: summary.p99 / 1000,
    client_cpu_percent: usage.cpuPercent,
    client_memory_mb: usage.memoryMb,
    server_cpu_percent: (server.cpuSeconds / usage.elapsedSeconds / LOGICAL_CORES) * 100,
    server_memory_mb: server.workingSetMb,
    client_cores: usage.cores,
    client_parallelism_ceiling: CLIENT_PARALLELISM_CEILING,
    peak_in_flight: peakInFlight,
    request_window: options.requestWindow,
    abandoned,
    errors,
    completed
  };
}

/**
 * `send-saturation`. spec section 5 / G3: throughput is what the SERVER received
 * during the active phase.
 *
 * FB-013: the snapshot is taken AT THE ACTIVE-WINDOW BOUNDARY. Reading it after
 * the drain counts messages that landed seconds after the window closed and
 * reports the client's submit rate as the server's consumption rate; on .NET
 * that inflated the framework row 4.2x and manufactured a 2.8x advantage over
 * gRPC that vanished once corrected. The drain still runs, purely as settle and
 * contamination detection (FB-008), and its observed time is reported per cell.
 */
async function runSendSaturation({ payloadSize, options, statsUrl, operation }) {
  for (let i = 0; i < options.warmup; i++) {
    await operation(payloadSize, header.PHASE_WARMUP, i);
  }
  await resetServer(statsUrl);

  const latencies = new Latencies(options.latencySampleLimit);
  let submitted = 0;
  let errors = 0;
  let inFlight = 0;
  let peakInFlight = 0;
  const resources = new ResourceSample();
  const startNs = header.nowNs();
  const activeUntilNs = startNs + BigInt(Math.round(options.durationSeconds * 1e9));

  const worker = async () => {
    while (header.nowNs() < activeUntilNs) {
      const index = submitted++;
      const t0 = header.nowNs();
      inFlight += 1;
      if (inFlight > peakInFlight) peakInFlight = inFlight;
      try {
        await operation(payloadSize, header.PHASE_ACTIVE, index);
      } catch (error) {
        errors += 1;
      } finally {
        inFlight -= 1;
      }
      latencies.add(elapsedMicros(t0));
    }
  };

  const workers = [];
  for (let slot = 0; slot < options.sendConcurrency; slot++) workers.push(worker());
  await Promise.all(workers);

  const usage = resources.finish();
  const boundary = await serverStats(statsUrl);
  const outcome = await waitForServerDrain(statsUrl, options.commandSettleMs, options.drainBoundMs);
  const postDrain = outcome.snapshot || boundary;

  const seconds = Math.max(1e-9, options.durationSeconds);
  const throughput = boundary.activeMessages / seconds;
  const clientSummary = latencies.summary();
  return {
    throughput_per_second: throughput,
    bandwidth_mb_s: (throughput * payloadSize) / 1e6,
    // spec section 5: for send, the reported latency is the SERVER-side receive latency
    // computed from the header, not the client's submit-call duration.
    latency_mean_ms: boundary.meanMicros / 1000,
    latency_p95_ms: boundary.p95Micros / 1000,
    latency_p99_ms: boundary.p99Micros / 1000,
    client_cpu_percent: usage.cpuPercent,
    client_memory_mb: usage.memoryMb,
    server_cpu_percent: (boundary.cpuSeconds / usage.elapsedSeconds / LOGICAL_CORES) * 100,
    server_memory_mb: boundary.workingSetMb,
    client_cores: usage.cores,
    client_parallelism_ceiling: CLIENT_PARALLELISM_CEILING,
    peak_in_flight: peakInFlight,
    request_window: options.sendConcurrency,
    abandoned: 0,
    drain_ms: outcome.drainMs,
    drain_bound_hit: outcome.boundHit,
    server_received_at_close: boundary.activeMessages,
    server_received_post_drain: postDrain.activeMessages,
    errors,
    submitted,
    client_submit_latency_mean_ms: clientSummary.mean / 1000,
    client_submit_latency_p95_ms: clientSummary.p95 / 1000,
    client_submit_latency_p99_ms: clientSummary.p99 / 1000
  };
}

module.exports = {
  LOGICAL_CORES,
  CLIENT_PARALLELISM_CEILING,
  ResourceSample,
  Latencies,
  delay,
  resetServer,
  serverStats,
  waitForServerDrain,
  waitForRouteReady,
  runRequestSerial,
  runRequestWindow,
  runSendSaturation
};
