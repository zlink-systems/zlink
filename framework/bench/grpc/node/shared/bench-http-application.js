// SPDX-License-Identifier: MPL-2.0
'use strict';

const http = require('node:http');
const { nowNs } = require('./bench-metric-header');

const PATTERNS = new Set([
  'request-serial',
  'request-window',
  'request-backpressure',
  'send-saturation'
]);
const PHASES = new Set(['warmup', 'active']);
const TRIGGER_FIELDS = new Set([
  'runId',
  'cellId',
  'pattern',
  'payloadBytes',
  'phase',
  'durationMs',
  'requestWindow',
  'sendConcurrency'
]);

class BenchPhaseController {
  constructor(isReady, counters, workload) {
    this.isReady = isReady;
    this.counters = counters;
    this.workload = workload;
    this.phase = 'idle';
    this.failure = null;
    this.acknowledgements = new Map();
    this.lastTrigger = null;
  }

  start(request) {
    validateTrigger(request);
    const key = `${request.runId}/${request.cellId}/${request.phase}`;
    const existing = this.acknowledgements.get(key);
    if (existing !== undefined) return { statusCode: 200, reply: existing };
    if (!this.isReady()) return this.reject(request, 'Source is not ready.');
    if (this.phase !== 'idle') {
      return this.reject(request, `Phase ${this.phase} has not completed.`);
    }

    this.phase = request.phase;
    this.failure = null;
    this.lastTrigger = {
      ...request,
      receivedAtUnixMs: Date.now()
    };
    const reply = {
      accepted: true,
      runId: request.runId,
      cellId: request.cellId,
      phase: request.phase,
      startedAt: Number(nowNs())
    };
    this.acknowledgements.set(key, reply);
    Promise.resolve()
      .then(() => this.workload(request))
      .then(
        () => { this.phase = 'idle'; },
        (error) => {
          this.failure = `${error && error.name ? error.name : 'Error'}: ${error && error.message}`;
          this.phase = 'failed';
        }
      );
    return { statusCode: 200, reply };
  }

  reject(request, reason) {
    return {
      statusCode: 409,
      reply: {
        accepted: false,
        runId: request.runId,
        cellId: request.cellId,
        phase: request.phase,
        startedAt: Number(nowNs()),
        reason
      }
    };
  }

  snapshot() {
    const values = this.counters();
    return {
      ready: this.isReady(),
      phase: this.phase,
      submitted: values.submitted,
      completed: values.completed,
      errors: values.errors,
      received: values.received || 0,
      inFlight: values.inFlight,
      currentInFlight: values.inFlight,
      peakInFlight: values.peakInFlight,
      failure: this.failure
    };
  }
}

function validateTrigger(request) {
  if (request === null || typeof request !== 'object' || Array.isArray(request)) {
    throw new TypeError('trigger body must be a JSON object');
  }
  const keys = Object.keys(request);
  const missing = [...TRIGGER_FIELDS].filter((name) => !keys.includes(name));
  const unknown = keys.filter((name) => !TRIGGER_FIELDS.has(name));
  if (missing.length > 0) throw new TypeError(`trigger missing ${missing.join(', ')}`);
  if (unknown.length > 0) throw new TypeError(`trigger has unknown fields: ${unknown.join(', ')}`);
  if (typeof request.runId !== 'string' || request.runId.trim() === ''
      || typeof request.cellId !== 'string' || request.cellId.trim() === '') {
    throw new TypeError('runId and cellId must be non-empty strings');
  }
  if (!PATTERNS.has(request.pattern)) throw new TypeError('unknown benchmark pattern');
  if (!PHASES.has(request.phase)) throw new TypeError('phase must be warmup or active');
  for (const name of ['payloadBytes', 'durationMs', 'requestWindow', 'sendConcurrency']) {
    if (!Number.isInteger(request[name]) || request[name] <= 0) {
      throw new TypeError(`${name} must be a positive integer`);
    }
  }
  if (request.payloadBytes < 29) throw new TypeError('payloadBytes must be at least 29');
}

function json(res, statusCode, value) {
  res.writeHead(statusCode, { 'content-type': 'application/json' });
  res.end(JSON.stringify(value));
}

function readJson(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let length = 0;
    req.on('data', (chunk) => {
      length += chunk.length;
      if (length > 65536) {
        reject(new TypeError('trigger body is too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')));
      } catch (error) {
        reject(new TypeError(`invalid trigger JSON: ${error.message}`));
      }
    });
    req.on('error', reject);
  });
}

function listen(url, handler) {
  const parsed = new URL(url);
  return new Promise((resolve, reject) => {
    const server = http.createServer(handler);
    server.once('error', reject);
    server.listen(Number(parsed.port), parsed.hostname, () => {
      server.removeListener('error', reject);
      resolve(server);
    });
  });
}

async function startSourceHttp(triggerUrl, statsUrl, controller) {
  const trigger = await listen(triggerUrl, async (req, res) => {
    const path = (req.url || '').split('?')[0];
    if (req.method !== 'POST' || path !== '/bench/start') {
      res.writeHead(404);
      res.end();
      return;
    }
    try {
      const request = await readJson(req);
      const outcome = controller.start(request);
      json(res, outcome.statusCode, outcome.reply);
    } catch (error) {
      json(res, 400, { reason: error.message });
    }
  });
  try {
    const stats = await listen(statsUrl, (req, res) => {
      const path = (req.url || '').split('?')[0];
      if (req.method === 'GET' && path === '/bench/stats') {
        json(res, 200, controller.snapshot());
        return;
      }
      res.writeHead(404);
      res.end();
    });
    return {
      close: () => Promise.all([closeServer(trigger), closeServer(stats)])
    };
  } catch (error) {
    await closeServer(trigger);
    throw error;
  }
}

function closeServer(server) {
  return new Promise((resolve) => server.close(() => resolve()));
}

module.exports = { BenchPhaseController, startSourceHttp, validateTrigger };
