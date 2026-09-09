// SPDX-License-Identifier: MPL-2.0
'use strict';

const http = require('node:http');
const header = require('./bench-metric-header');

/**
 * What a bench server counts, and the /bench/stats endpoint the client polls.
 *
 * spec section 5 / G3: `send-saturation` throughput is the number of messages the
 * SERVER received during the active phase, never the client's submit count.
 * That is why every server carries this and why the counter only advances for
 * payloads whose header says phase == active.
 */
class BenchServerMetrics {
  constructor() {
    this._activeMessages = 0;
    this._errors = 0;
    this._latencyNs = [];
    this._latencyLimit = 500000;
    this._cpuStart = process.cpuUsage();
    this._wallStartNs = header.nowNs();
  }

  reset() {
    this._activeMessages = 0;
    this._errors = 0;
    this._latencyNs = [];
    this._cpuStart = process.cpuUsage();
    this._wallStartNs = header.nowNs();
  }

  /** `payload` is the stamped body, not the protobuf envelope. */
  record(payload) {
    const decoded = header.decode(payload);
    if (decoded === null || decoded.phase !== header.PHASE_ACTIVE) return;
    this._activeMessages += 1;
    if (this._latencyNs.length < this._latencyLimit) {
      const latency = header.nowNs() - decoded.sentTimestampNs;
      this._latencyNs.push(latency > 0n ? Number(latency) : 0);
    }
  }

  recordError() {
    this._errors += 1;
  }

  snapshot() {
    const cpu = process.cpuUsage(this._cpuStart);
    const samples = this._latencyNs.slice().sort((a, b) => a - b);
    return {
      activeMessages: this._activeMessages,
      errors: this._errors,
      meanMicros: mean(samples) / 1000,
      p50Micros: percentile(samples, 0.5) / 1000,
      p95Micros: percentile(samples, 0.95) / 1000,
      p99Micros: percentile(samples, 0.99) / 1000,
      cpuSeconds: (cpu.user + cpu.system) / 1e6,
      workingSetMb: process.memoryUsage().rss / 1024 / 1024
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

/**
 * The stats endpoint. It must answer WHILE the server is still draining, because
 * the settle contract (spec section 3 / FB-008) is "poll until the received count
 * stops moving", not "sleep a fixed time".
 */
function startStatsServer(url, metrics, extra = {}) {
  const parsed = new URL(url);
  const server = http.createServer((req, res) => {
    const path = (req.url || '').split('?')[0];
    if (req.method === 'GET' && path === '/ready') {
      res.writeHead(200, { 'content-type': 'text/plain' });
      res.end('ready');
      return;
    }
    if (req.method === 'POST' && path === '/bench/reset') {
      metrics.reset();
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end('{}');
      return;
    }
    if (req.method === 'GET' && path === '/bench/stats') {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify(metrics.snapshot()));
      return;
    }
    if (req.method === 'GET' && path === '/bench/info') {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify(extra));
      return;
    }
    res.writeHead(404);
    res.end();
  });
  server.listen(Number(parsed.port), parsed.hostname);
  return server;
}

module.exports = { BenchServerMetrics, startStatsServer };
