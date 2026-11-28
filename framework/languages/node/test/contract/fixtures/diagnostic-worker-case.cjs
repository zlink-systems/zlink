'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const { join } = require('node:path');
const { setImmediate: nextTurn, setTimeout: delay } = require('node:timers/promises');
const { threadId } = require('node:worker_threads');

const runtime = join(__dirname, '../../../packages/framework/dist/runtime');
const delivery = require(join(runtime, 'diagnostics/message-flow-delivery'));
const { logDiagnostic } = require(join(runtime, 'diagnostics/diagnostic-log'));
const { TelemetryState, TelemetryCounter, MESSAGE_FLOW_PENDING_LIMIT } = delivery;
const mode = process.env.ZLINK_TEST_TELEMETRY_CASE;
const evidenceFile = process.env.ZLINK_TEST_TELEMETRY_RECORDS;
const gateFile = process.env.ZLINK_TEST_TELEMETRY_GATE;
const keepAlive = setInterval(() => {}, 1000);
const owner = delivery.messageFlowDelivery();

function evidence() {
  if (!fs.existsSync(evidenceFile)) return [];
  return fs.readFileSync(evidenceFile, 'utf8').split('\n').filter(Boolean).map(JSON.parse);
}

async function until(predicate) {
  const deadline = performance.now() + 10000;
  while (!predicate()) {
    assert.ok(performance.now() < deadline, 'diagnostic delivery observation deadline');
    await delay(5);
  }
}

async function progress() {
  assert.equal(owner.enabled(), false, 'console diagnostics do not require an OpenTelemetry provider');
  const before = process.env.ZLINK_TEST_DIAGNOSTIC_BASELINE === '1';
  const suffix = before ? '.before-5996.cjs' : '.js';
  const { ZLinkRouteMeshRuntimeCoordinator } = require(join(runtime, `host/route-mesh-runtime${suffix}`));
  const { ZLinkHostServiceRelocationRuntime } = require(join(runtime, `host/service-relocation-host-runtime${suffix}`));
  const lease = delivery.openTelemetryLease();
  const markers = [
    '[zlink.runtime.relocation.drain_failed]',
    '[zlink.runtime.relocation.source_cleanup_failed]'
  ];
  const originalError = console.error;
  const mainWrites = [];
  console.error = (marker, ...args) => {
    if (!markers.includes(marker)) return originalError(marker, ...args);
    const start = performance.now();
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 500);
    const elapsed = performance.now() - start;
    mainWrites.push({ marker, threadId, elapsed });
  };

  const failure = new Error('diagnostic-probe failure');
  let rollbackAt;
  const route = new ZLinkRouteMeshRuntimeCoordinator({
    meshNames: ['mesh'],
    admission: { register() {} },
    diagnosticLease: () => lease,
    async drainResources() { throw failure; },
    async rollbackRetiring() { rollbackAt = performance.now(); }
  });
  const source = new ZLinkHostServiceRelocationRuntime({
    diagnosticLease: () => lease,
    actorTransfer: { async completeRelocationSourceLeave() { throw failure; } },
    actorManager: () => undefined
  });
  const profile = { ready: Promise.resolve(), actor: {}, sourceSpotId: 'spot' };
  source.sourceActorJoinProfiles.set('relocation', profile);

  const elapsed = [];
  try {
    process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = '1';
    let started = performance.now();
    const result = await route.relocateHost(3000);
    assert.equal(result.kind, 'forceStopped');
    assert.equal(result.reason, 'teardown_failed');
    elapsed.push({ marker: markers[0], operationMs: rollbackAt - started, nextTurnMs: await nextTurn().then(() => performance.now() - started) });
    process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = '0';
    started = performance.now();
    await source.finalizeActorJoinSource(profile, 'relocation');
    elapsed.push({ marker: markers[1], operationMs: performance.now() - started, nextTurnMs: await nextTurn().then(() => performance.now() - started) });
    await route.relocateHost(3000);
  } finally {
    console.error = originalError;
  }
  console.log(JSON.stringify({ elapsed, mainWrites }));
  assert.ok(elapsed.every(({ operationMs, nextTurnMs }) => operationMs < 250 && nextTurnMs < 250),
    'each operation and its next main-thread turn must complete within 250 ms');
  assert.deepEqual(mainWrites, []);
  await until(() => evidence().filter(({ kind }) => kind === 'diagnostic-write').length >= 2);
  const writes = evidence().filter(({ kind }) => kind === 'diagnostic-write');
  assert.deepEqual(writes.map(({ thread }) => thread), ['worker', 'worker']);
  for (const [index, row] of writes.entries()) {
    assert.ok(row.value.text.startsWith(markers[index]));
    assert.match(row.value.text, /diagnostic-probe failure/);
    assert.match(row.value.text, /level: 'error'/);
    assert.match(row.value.text, /category: 'relocation'/);
    assert.ok(row.value.elapsed >= 500 && row.value.threadId !== 0);
  }
  assert.equal(Atomics.load(owner.counters, TelemetryCounter.DiagnosticOverflow), 0n);
  console.log(JSON.stringify(writes));
  console.log('diagnostic overflow=0');
}

async function overflow() {
  const live = delivery.openTelemetryLease();
  const retired = delivery.openTelemetryLease();
  logDiagnostic(live, 'warn', 'test', '[diagnostic.blocked]');
  logDiagnostic(retired, 'warn', 'test', '[diagnostic.retired-in-transit]');
  logDiagnostic(live, 'warn', 'test', '[diagnostic.survivor]');
  await until(() => evidence().some(({ kind }) => kind === 'diagnostic-entered'));
  assert.equal(Atomics.load(owner.state, TelemetryState.Pending), 2);
  for (let i = 2; i < MESSAGE_FLOW_PENDING_LIMIT; i++) {
    logDiagnostic(retired, 'warn', 'test', '[diagnostic.retired-pending]');
  }
  assert.equal(Atomics.load(owner.state, TelemetryState.Pending), MESSAGE_FLOW_PENDING_LIMIT);
  for (let i = 0; i < 3; i++) logDiagnostic(live, 'warn', 'test', '[diagnostic.dropped]');
  owner.submit(live, {
    eventId: 'zlink.message_flow', timestamp: new Date(), phase: 'received',
    outcome: 'succeeded', surface: 'channel', messageKind: 'request'
  });
  assert.equal(Atomics.load(owner.counters, TelemetryCounter.DiagnosticOverflow), 3n);
  assert.equal(Atomics.load(owner.counters, TelemetryCounter.Overflow), 1n);
  delivery.retireTelemetryLease(retired);
  logDiagnostic(retired, 'warn', 'test', '[diagnostic.late-retired]');
  assert.equal(Atomics.load(owner.state, TelemetryState.Pending), 2);
  fs.writeFileSync(gateFile, 'release');
  await until(() => owner.workerReady && !owner.executing);
  const rows = evidence();
  assert.deepEqual(rows.filter(({ kind }) => kind === 'diagnostic-write')
    .map(({ thread, value }) => [thread, value.text.split(' ')[0]]),
  [['worker', '[diagnostic.blocked]'], ['worker', '[diagnostic.survivor]']]);
  assert.deepEqual(rows.filter(({ kind }) => kind === 'metric')
    .map(({ thread, value }) => [thread, value.attributes.source, value.value]),
  [['worker', 'message_flow', 1], ['worker', 'diagnostic', 3]]);
  console.log(JSON.stringify(rows.filter(({ kind }) => kind === 'metric')));
}

(async () => {
  await owner.ready;
  if (mode === 'diagnostic-progress') await progress();
  else await overflow();
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
}).finally(() => clearInterval(keepAlive));
