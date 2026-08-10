#!/usr/bin/env node
// Single-owner admission-burst benchmark for the bounded Spot scheduler.
//
// The first turn is held so one physical application FIFO reaches its count
// boundary. The workload intentionally submits past both the historical 4096
// limit and the canonical 1024 limit, then measures drain and recovery with the
// same payload cost in every run.

const path = require('node:path');
const { performance } = require('node:perf_hooks');

const {
  ZLinkBoundedSerialScheduler,
  ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS
} = require(path.resolve(
  __dirname,
  '../packages/framework/dist/runtime/execution/serial-scheduler'
));
const {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind
} = require(path.resolve(
  __dirname,
  '../packages/framework/dist/runtime/framework-errors-internal'
));
const {
  ZLinkFrameworkErrorKind
} = require(path.resolve(
  __dirname,
  '../packages/framework/dist/contracts/Errors/ZLinkFrameworkException'
));

const ATTEMPTS = Number(process.env.PERF_ATTEMPTS ?? 4_352);
const PAYLOAD_BYTES = Number(process.env.PERF_PAYLOAD_BYTES ?? 1_024);
const ROUNDS = Number(process.env.PERF_ROUNDS ?? 5);

function deferred() {
  let resolve;
  const promise = new Promise((settle) => {
    resolve = settle;
  });
  return { promise, resolve };
}

function percentile(sortedValues, ratio) {
  const index = Math.min(sortedValues.length - 1, Math.ceil(sortedValues.length * ratio) - 1);
  return sortedValues[Math.max(0, index)];
}

function errorMapping(error) {
  const publicKind = typeof error?.kind === 'number'
    ? (ZLinkFrameworkErrorKind[error.kind] ?? String(error.kind))
    : 'none';
  const internalKind = error?.name === 'ZLinkFrameworkException'
    ? (internalFrameworkErrorKind(error) ?? 'none')
    : 'none';
  return `${error?.name ?? typeof error}/${internalKind}/${publicKind}`;
}

async function runRound() {
  const gate = deferred();
  const latencies = [];
  const accepted = [];
  let firstRejection;
  let firstRejectionOrdinal;
  const scheduler = new ZLinkBoundedSerialScheduler(
    async (record) => {
      try {
        record.resolve(await record.operation());
      } catch (error) {
        record.reject(error);
      } finally {
        record.release();
      }
    },
    {
      capacityError: () => createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.WorkerQueueFull,
        'Spot execution queue capacity was exceeded.'
      )
    }
  );

  const submissions = [];
  for (let index = 0; index < ATTEMPTS; index += 1) {
    const startedAt = performance.now();
    const operation = index === 0 ? () => gate.promise : () => undefined;
    const submission = scheduler.submit(operation, { payloadBytes: PAYLOAD_BYTES }).then(
      () => {
        latencies.push(performance.now() - startedAt);
        return true;
      },
      (error) => {
        if (firstRejection === undefined) {
          firstRejection = error;
          firstRejectionOrdinal = index + 1;
        }
        return false;
      }
    );
    submissions.push(submission);
  }

  // Admission is synchronous. The Promise turn lets rejection handlers record
  // the error before the held owner is released.
  await Promise.resolve();
  const peak = scheduler.snapshot();
  if (firstRejection === undefined) {
    throw new Error(`workload did not cross the queue boundary: attempts=${ATTEMPTS}`);
  }

  const drainStartedAt = performance.now();
  gate.resolve();
  await submissions[0];

  const recoveryStartedAt = performance.now();
  const recovery = scheduler.submit(() => undefined, { payloadBytes: PAYLOAD_BYTES });
  const recoveryAdmittedAt = performance.now();
  await recovery;
  const recoveryCompletedAt = performance.now();

  const settled = await Promise.all(submissions);
  await scheduler.whenIdle();
  const drainCompletedAt = performance.now();
  for (const value of settled) {
    if (value) accepted.push(value);
  }
  latencies.sort((left, right) => left - right);

  const wallMs = drainCompletedAt - drainStartedAt;
  return {
    acceptedCount: accepted.length,
    rejectedCount: ATTEMPTS - accepted.length,
    wallMs,
    throughput: accepted.length / (wallMs / 1_000),
    p95: percentile(latencies, 0.95),
    p99: percentile(latencies, 0.99),
    peakPending: peak.applicationMessages,
    peakRetainedBytes: peak.applicationBytes,
    firstRejectionOrdinal,
    firstRejectionMapping: errorMapping(firstRejection),
    recoveryAdmissionMs: recoveryAdmittedAt - recoveryStartedAt,
    recoveryCompletionMs: recoveryCompletedAt - recoveryStartedAt,
    finalSnapshot: scheduler.snapshot()
  };
}

async function main() {
  if (!Number.isSafeInteger(ATTEMPTS) || ATTEMPTS <= 4_096) {
    throw new RangeError('PERF_ATTEMPTS must be a safe integer greater than 4096.');
  }
  if (!Number.isSafeInteger(PAYLOAD_BYTES) || PAYLOAD_BYTES < 0) {
    throw new RangeError('PERF_PAYLOAD_BYTES must be a non-negative safe integer.');
  }
  if (!Number.isSafeInteger(ROUNDS) || ROUNDS < 1) {
    throw new RangeError('PERF_ROUNDS must be a positive safe integer.');
  }

  const results = [];
  for (let round = 0; round < ROUNDS; round += 1) {
    results.push(await runRound());
  }

  const median = (selector) => {
    const values = results.map(selector).sort((left, right) => left - right);
    return values[Math.floor(values.length / 2)];
  };
  const defaults = ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS;
  console.log('entry-spot admission burst (single bounded application FIFO)');
  console.log(
    `attempts=${ATTEMPTS} payloadBytes=${PAYLOAD_BYTES} rounds=${ROUNDS} ` +
    `capacity=${defaults.applicationMessageCapacity}/${defaults.applicationByteCapacity} ` +
    `ownerBudgetMs=${defaults.ownerTimeBudgetMs}`
  );
  for (const [index, result] of results.entries()) {
    console.log(
      `round ${index + 1}: accepted=${result.acceptedCount} rejected=${result.rejectedCount} ` +
      `wall=${result.wallMs.toFixed(2)}ms throughput=${Math.round(result.throughput)}/s ` +
      `p95=${result.p95.toFixed(3)}ms p99=${result.p99.toFixed(3)}ms ` +
      `peakPending=${result.peakPending} peakRetainedBytes=${result.peakRetainedBytes} ` +
      `firstRejection=${result.firstRejectionOrdinal}:${result.firstRejectionMapping} ` +
      `recoveryAdmission=${result.recoveryAdmissionMs.toFixed(3)}ms ` +
      `recoveryCompletion=${result.recoveryCompletionMs.toFixed(3)}ms`
    );
    if (Object.values(result.finalSnapshot).some((value) => value !== 0)) {
      throw new Error(`scheduler did not release every reservation: ${JSON.stringify(result.finalSnapshot)}`);
    }
  }
  console.log(
    `median: wall=${median((r) => r.wallMs).toFixed(2)}ms ` +
    `throughput=${Math.round(median((r) => r.throughput))}/s ` +
    `p95=${median((r) => r.p95).toFixed(3)}ms ` +
    `p99=${median((r) => r.p99).toFixed(3)}ms ` +
    `peakPending=${median((r) => r.peakPending)} ` +
    `peakRetainedBytes=${median((r) => r.peakRetainedBytes)} ` +
    `firstRejection=${median((r) => r.firstRejectionOrdinal)} ` +
    `recoveryAdmission=${median((r) => r.recoveryAdmissionMs).toFixed(3)}ms ` +
    `recoveryCompletion=${median((r) => r.recoveryCompletionMs).toFixed(3)}ms`
  );
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
