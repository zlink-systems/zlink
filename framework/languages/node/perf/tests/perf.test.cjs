const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { FanoutMetrics } = require('../build/server-support/fanout-metrics.js');
const { PerfClock } = require('../build/shared/clock.js');
const { PerfPublishEvent } = require('../build/shared/contracts.js');
const { classifyError } = require('../build/shared/errors.js');
const { Histogram, HISTOGRAM_BOUNDS } = require('../build/shared/histogram.js');
const { Measurement } = require('../build/shared/measurement.js');
const { PayloadPattern } = require('../build/shared/payload.js');
const { ScenarioMetrics } = require('../build/server-support/scenario-metrics.js');
const { SendSendCorrelation } = require('../build/server-support/send-send-correlation.js');
const { S2sSpotToChannelSendSendEchoScenario } = require('../build/spot-server/s2s-spot-to-channel-send-send-echo-scenario.js');
const { FanoutReceipts } = require('../build/subscriber-server/fanout-receipts.js');
const { ObjectsReadiness } = require('../build/server-support/server-application.js');
const { SessionActorSetup } = require('../build/server-support/actor-echo-support.js');
const { S2sSpotToChannelRequestEchoScenario } = require('../build/spot-server/s2s-spot-to-channel-request-echo-scenario.js');
const { SpotWorkerOffloadHandler } = require('../build/spot-server/spot-worker-offload-echo-scenario.js');

function config(overrides = {}) {
  return {
    runId: 'run', cellId: 'cell', configHash: 'hash', role: 'spot', roleInstance: 0, provenance: {},
    workload: {
      payloadSize: 32, warmupSeconds: 1, durationSeconds: 1, setupTimeoutMs: 100, requestTimeoutMs: 50,
      driverTimeoutMs: 1234, connections: null, clientCount: 1, logicalStreams: 1, inflight: 1
    },
    ...overrides
  };
}

function openMeasurement(roleConfig = config(), primary = true) {
  const measurement = new Measurement(roleConfig, primary);
  const now = PerfClock.now();
  measurement.start = now;
  measurement.end = now + 1_000_000_000n;
  measurement.currentResetSeq = '1';
  measurement.currentPhase = 'measured';
  return measurement;
}

test('terminal at endTicks is inflightAtEnd and the outcome equation balances', () => {
  const measurement = openMeasurement();
  const started = measurement.beginOperation();
  assert.notEqual(started, undefined);

  assert.equal(measurement.completeOperation(started, undefined, measurement.endTicks), false);
  const metrics = measurement.snapshot(null).metrics;
  assert.equal(metrics['messages.sent'], '1');
  assert.equal(metrics['messages.completed'], '0');
  assert.equal(metrics['messages.failed'], '0');
  assert.equal(metrics['messages.timeout'], '0');
  assert.equal(metrics['messages.cancelled'], '0');
  assert.equal(metrics['messages.inflightAtEnd'], '1');
  assert.equal(Number(metrics['messages.sent']), ['completed', 'failed', 'timeout', 'cancelled', 'inflightAtEnd']
    .reduce((total, key) => total + Number(metrics[`messages.${key}`]), 0));
  measurement.dispose();
});

test('worker warmup evidence is staged without recording a measured histogram sample', async () => {
  const roleConfig = config({ source: true, terminal: 'ordinary', worker: { taskMillis: 1, workerTimeoutMs: 100 } });
  const measurement = openMeasurement(roleConfig);
  measurement.currentPhase = 'warmup';
  const metrics = new ScenarioMetrics(measurement).latency('workerCallLatencyMs', 'worker.callLatency');
  const timings = new Map();
  const observation = { startedTicks: '100', endedTicks: '200', clockDomainId: 'worker-thread-hrtime', iterations: '1024', checksum: 7 };
  const call = { timeoutMs() { return this; }, submit: async () => observation, yield: async () => observation };
  const handler = new SpotWorkerOffloadHandler(measurement, metrics, roleConfig, timings);
  const request = measurement.request(0, 1, true);
  const spot = { context: { runCpuWorker: () => call } };

  await handler.handle(spot, request, {});
  assert.equal(timings.has(request.correlationId), true);

  const started = measurement.beginOperation();
  assert.notEqual(started, undefined);
  const primarySucceeded = measurement.completeOperation(started, undefined, started + 1_000n);
  const worker = timings.get(request.correlationId);
  timings.delete(request.correlationId);
  if (primarySucceeded && measurement.phase === 'measured')
    metrics.record('workerCallLatencyMs', worker.submitted, worker.resumed, started + 1_000n);
  assert.equal(measurement.snapshot(null).histograms.workerCallLatencyMs.count, '0');
  measurement.dispose();
});

test('auxiliary worker samples are admitted only after a primary in-window success', () => {
  const measurement = openMeasurement();
  const metrics = new ScenarioMetrics(measurement).latency('workerTaskLatencyMs', 'worker.taskLatency');
  const started = measurement.beginOperation();
  assert.notEqual(started, undefined);

  const ended = started + 20_000n;
  const succeeded = measurement.completeOperation(started, undefined, ended);
  if (succeeded) metrics.record('workerTaskLatencyMs', started + 1n, ended, ended);
  let snapshot = measurement.snapshot(null);
  assert.equal(snapshot.histograms.workerTaskLatencyMs.count, '1');

  const lateMeasurement = openMeasurement();
  const lateMetrics = new ScenarioMetrics(lateMeasurement).latency('workerTaskLatencyMs', 'worker.taskLatency');
  const lateStarted = lateMeasurement.beginOperation();
  assert.notEqual(lateStarted, undefined);
  const lateSuccess = lateMeasurement.completeOperation(lateStarted, undefined, lateMeasurement.endTicks);
  if (lateSuccess) lateMetrics.record('workerTaskLatencyMs', lateStarted, lateMeasurement.endTicks, lateMeasurement.endTicks);
  snapshot = lateMeasurement.snapshot(null);
  assert.equal(snapshot.histograms.workerTaskLatencyMs.count, '0');
  measurement.dispose();
  lateMeasurement.dispose();
});

test('histogram percentile is capped at the observed maximum and overflow reports the last bound', () => {
  const histogram = new Histogram();
  histogram.record(20_000n);
  const metrics = {};
  const histograms = {};
  const reasons = {};
  histogram.export('latencyMs', 'latency', metrics, histograms, reasons);
  assert.equal(metrics['latency.p50Ms'], 0.02);
  assert.equal(histograms.latencyMs.percentileMethod, 'nearest-rank-bucket-upper-bound-capped-by-max');

  const overflow = new Histogram();
  overflow.record(BigInt((HISTOGRAM_BOUNDS.at(-1) + 1) * 1_000_000));
  const overflowReasons = {};
  overflow.export('latencyMs', 'latency', {}, {}, overflowReasons);
  assert.equal(overflowReasons['/metrics/latency.p50Ms'].lowerBoundMs, HISTOGRAM_BOUNDS.at(-1));
});

test('driver submit failure is counted as driver.failed without a measured operation outcome', async () => {
  const roleConfig = config({
    role: 'spot', source: true, spotIds: ['spot-0'],
    workload: { ...config().workload, logicalStreams: 1, inflight: 1, driverTimeoutMs: 1234 }
  });
  const measurement = openMeasurement(roleConfig);
  let issueChecks = 0;
  Object.defineProperty(measurement, 'canIssue', { get: () => issueChecks++ === 0 });
  const metrics = new ScenarioMetrics(measurement)
    .counters('driver.issued', 'driver.notStarted', 'driver.failed', 'spot.applicationHandlerEntries')
    .latency('driverLatencyMs', 'driver.latency');
  const spots = {
    requestToSpot(_spotId, _request) {
      return {
        timeout(timeoutMs) {
          assert.equal(timeoutMs, 1234);
          return { submit: async () => { throw new Error('driver call failed'); } };
        }
      };
    }
  };
  const scenario = new S2sSpotToChannelRequestEchoScenario(spots, {}, measurement, roleConfig, {}, new ObjectsReadiness(false, ''), metrics);
  scenario.sequences = [0];
  await scenario.loop(0);
  const snapshot = measurement.snapshot(null);
  assert.equal(snapshot.metrics['driver.failed'], '1');
  assert.equal(snapshot.metrics['messages.sent'], '0');
  assert.equal(snapshot.metrics['messages.failed'], '0');
  assert.equal(snapshot.histograms.driverLatencyMs.count, '0');
  measurement.dispose();
});

test('request driver uses handler echo as success without second validation', async () => {
  const roleConfig = config({
    role: 'spot', source: true, spotIds: ['spot-0'],
    workload: { ...config().workload, logicalStreams: 1, inflight: 1, driverTimeoutMs: 1234 }
  });
  const measurement = openMeasurement(roleConfig);
  let issueChecks = 0;
  Object.defineProperty(measurement, 'canIssue', { get: () => issueChecks++ === 0 });
  const metrics = new ScenarioMetrics(measurement)
    .counters('driver.issued', 'driver.notStarted', 'driver.failed', 'spot.applicationHandlerEntries')
    .latency('driverLatencyMs', 'driver.latency');
  const spots = {
    requestToSpot() {
      return {
        timeout() {
          return { submit: async () => ({ started: true, echo: { runId: 'invalid-handler-output' } }) };
        }
      };
    }
  };
  const scenario = new S2sSpotToChannelRequestEchoScenario(spots, {}, measurement, roleConfig, {}, new ObjectsReadiness(false, ''), metrics);
  scenario.sequences = [0];
  await scenario.loop(0);
  const snapshot = measurement.snapshot(null);
  assert.equal(snapshot.metrics['driver.failed'], '0');
  assert.equal(snapshot.histograms.driverLatencyMs.count, '1');
  assert.deepEqual(snapshot.metrics['errors.harness'], {});
  measurement.dispose();
});

test('send/send driver latency ends when submit returns and uses primary completion only as the window gate', async () => {
  const roleConfig = config({
    role: 'spot', source: true, terminal: 'ordinary', spotIds: ['spot-0'],
    workload: { ...config().workload, logicalStreams: 1, inflight: 1, requestTimeoutMs: 617, driverTimeoutMs: 1234 }
  });
  const measurement = openMeasurement(roleConfig);
  let issueChecks = 0;
  Object.defineProperty(measurement, 'canIssue', { get: () => issueChecks++ === 0 });
  const operationStarted = measurement.beginOperation('send');
  assert.notEqual(operationStarted, undefined);
  const originalNow = PerfClock.now;
  let fakeNow = operationStarted + 100n;
  PerfClock.now = () => fakeNow;
  const metrics = new ScenarioMetrics(measurement)
    .counters('driver.issued', 'driver.notStarted', 'driver.failed', 'spot.applicationHandlerEntries')
    .latency('driverLatencyMs', 'driver.latency');
  const spots = {
    requestToSpot() {
      return {
        timeout(timeoutMs) {
          assert.equal(timeoutMs, 1234);
          return { submit: async () => { fakeNow += 100n; return { started: true }; } };
        }
      };
    }
  };
  const correlations = {
    find: () => ({ startedTicks: operationStarted }),
    completeAsync: async () => {
      fakeNow += 1_000n;
      return { error: undefined, completedTicks: fakeNow };
    }
  };
  const scenario = new S2sSpotToChannelSendSendEchoScenario(spots, {}, measurement, roleConfig, {},
    new ObjectsReadiness(false, ''), metrics, correlations);
  scenario.sequences = [0];
  try {
    await scenario.loop(0);
    const histogram = measurement.snapshot(null).histograms.driverLatencyMs;
    assert.equal(histogram.count, '1');
    assert.equal(histogram.maxNs, '100');
  } finally {
    PerfClock.now = originalNow;
    measurement.dispose();
  }
});

test('public AbortError is classified as cancelled', () => {
  const error = new Error('cancelled');
  error.name = 'AbortError';
  assert.equal(classifyError(error).category, 'cancelled');
});

test('unknown Framework error kind is a collection failure and retains its number in diagnostics', () => {
  const error = new Error('unknown Framework enum value');
  error.name = 'ZLinkFrameworkException';
  error.kind = 2_147_483_647;
  const classified = classifyError(error);
  assert.equal(classified.namespace, 'harness');
  assert.equal(classified.key, 'CollectionFailure');
  assert.equal(classified.harnessKind, 'CollectionFailure');
  assert.equal(classified.unrecognizedFrameworkKind, error.kind);

  const measurement = openMeasurement();
  measurement.recordDiagnostic(error);
  const snapshot = measurement.snapshot(null);
  assert.deepEqual(snapshot.metrics['errors.byKind'], {});
  assert.equal(snapshot.metrics['errors.harness'].CollectionFailure, '1');
  assert.equal(snapshot.runtimeMetrics.errors.value[0].unrecognizedFrameworkKind, error.kind);
  measurement.dispose();
});

test('Subscriber classifies receipt by handler entry tick at the half-open window end', () => {
  const measurement = openMeasurement(config({ role: 'subscriber' }), false);
  const receipts = new FanoutReceipts(measurement, config({ role: 'subscriber', channelName: 'fanout' }),
    new ObjectsReadiness(false, ''), os.tmpdir());
  const event = new PerfPublishEvent({
    runId: 'run', cellId: 'cell', resetSeq: '1', phase: 'measured', sequence: '1', topic: 'perf.echo',
    sentTicks: '1', clockDomainId: PerfClock.domain, payload: measurement.pattern.base64
  });
  receipts.record(event, measurement.endTicks);
  const delivery = measurement.snapshot(null).runtimeMetrics.fanoutReceipts.value;
  assert.equal(delivery.uniqueInWindow, '0');
  assert.equal(delivery.measuredOutsideWindow, '1');
  measurement.dispose();
});

test('Subscriber final snapshot does not add a second receipt classification rule', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-node-fanout-seal-'));
  const roleConfig = config({ role: 'subscriber', channelName: 'fanout' });
  const measurement = openMeasurement(roleConfig, false);
  const receipts = new FanoutReceipts(measurement, roleConfig, new ObjectsReadiness(false, ''), directory);
  const event = new PerfPublishEvent({
    runId: 'run', cellId: 'cell', resetSeq: '1', phase: 'measured', sequence: '7', topic: 'perf.echo',
    sentTicks: '1', clockDomainId: PerfClock.domain, payload: measurement.pattern.base64
  });
  try {
    measurement.currentPhase = 'complete';
    measurement.finalSnapshot = true;
    measurement.snapshot(null);
    measurement.finalSnapshot = false;
    receipts.record(event, measurement.startTicks);
    const delivery = measurement.snapshot(null).runtimeMetrics.fanoutReceipts.value;
    assert.equal(delivery.uniqueInWindow, '1');
  } finally {
    measurement.dispose();
    fs.rmSync(directory, { recursive: true });
  }
});

test('channel send/send validates the return SpotId selected by clientId', () => {
  const roleConfig = config({ role: 'channel' });
  const measurement = openMeasurement(roleConfig, false);
  const request = measurement.request(0, 1, true).with({ returnSpotId: 'source-spot-0' });
  assert.doesNotThrow(() => measurement.validateRequest(request, null, 'source-spot-0'));
  assert.throws(() => measurement.validateRequest(request, null, 'source-spot-1'));
  measurement.dispose();
});

test('send/send reply after its fixed deadline is expired even before first-send completion', async () => {
  const roleConfig = config({ workload: { ...config().workload, correlationExpiryMs: 10 } });
  const measurement = openMeasurement(roleConfig);
  const metrics = new ScenarioMetrics(measurement);
  const correlations = new SendSendCorrelation(measurement, metrics);
  const request = measurement.request(0, 1);
  const entry = correlations.register(request, measurement.startTicks);
  await new Promise((resolve) => setTimeout(resolve, 25));
  correlations.reply(PayloadPattern.reply(request, PerfClock.now()));

  const result = await correlations.completeAsync(entry);
  assert.equal(result.error.kind, 'CorrelationExpired');
  assert.equal(measurement.snapshot(null).metrics['messages.expired'], '1');
  assert.equal(measurement.snapshot(null).metrics['messages.lateReply'], '1');
  measurement.dispose();
});

test('sequence writer reports EEXIST and preserves the previous original', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-node-perf-sequences-'));
  try {
    const originalPath = path.join(directory, 'publisher-sequences.json');
    fs.writeFileSync(originalPath, '{"previous":true}\n');
    assert.throws(() => FanoutMetrics.writeOnce(originalPath, { runId: 'run' }), { code: 'EEXIST' });
    assert.equal(fs.readFileSync(originalPath, 'utf8'), '{"previous":true}\n');
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

async function failSessionActorBind() {
  const roleConfig = config({ role: 'session', actorIds: ['actor-0'], meshName: 'mesh-0', workload: { ...config().workload, setupTimeoutMs: 100 } });
  const measurement = new Measurement(roleConfig, false);
  const actors = {
    getOrCreate() {
      return { inMesh() { return this; }, timeout() { return this; }, submit: async () => ({ status: 'created', actor: {} }) };
    }
  };
  const readiness = new ObjectsReadiness(true, 'Session role objects are not applicable.');
  const setup = SessionActorSetup.length === 4
    ? new SessionActorSetup(roleConfig, measurement, readiness, actors)
    : new SessionActorSetup(roleConfig, measurement, actors);
  const session = { actors: { bindOrGet: async () => { throw new Error('bind failed'); } } };
  await assert.rejects(setup.prepare(session, { decode: () => ({ clientId: 0 }) }), /bind failed/);
  return { measurement, readiness };
}

test('session actor bind failure does not set objectsReady from one bind attempt', async () => {
  const { measurement, readiness } = await failSessionActorBind();
  assert.equal(readiness.ready, true);
  measurement.dispose();
});

test('session actor bind failure remains in the failure snapshot without proving consumer readiness', async () => {
  const { measurement } = await failSessionActorBind();
  const snapshot = measurement.snapshot(null);
  assert.deepEqual(snapshot.runtimeMetrics.preparationEvidence.value.actorCreateAndBind.observedValue, {
    created: 0, existing: 0, bound: 0, failed: 1, expectedActors: 1,
    createMeanMs: 0, createMaxMs: 0, bindMeanMs: 0, bindMaxMs: 0
  });
  assert.deepEqual(snapshot.runtimeMetrics.setupEvidence.value, []);
  measurement.dispose();
});
