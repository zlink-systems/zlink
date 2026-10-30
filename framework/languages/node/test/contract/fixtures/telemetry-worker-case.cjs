'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { setTimeout: delay } = require('node:timers/promises');
const modules = process.env.ZLINK_TEST_TELEMETRY_MODULE_DIR;
const flow = require(path.join(modules, 'message-flow.js'));
const delivery = require(path.join(modules, 'message-flow-delivery.js'));
const { TelemetryState: State, TelemetryCounter: Counter } = delivery;
const mode = process.env.ZLINK_TEST_TELEMETRY_CASE;
const originalNodeOptions = process.env.NODE_OPTIONS;
const keepAlive = setInterval(() => {}, 1000);

function rows() {
  return fs.existsSync(process.env.ZLINK_TEST_TELEMETRY_RECORDS)
    ? fs.readFileSync(process.env.ZLINK_TEST_TELEMETRY_RECORDS, 'utf8').trim().split('\n').filter(Boolean).map(JSON.parse) : [];
}

async function until(predicate) {
  const deadline = performance.now() + 10000;
  while (!predicate()) {
    assert.ok(performance.now() < deadline, 'telemetry observation deadline');
    await delay(5);
  }
}

function makeTracer(messageFlow = 'normal', sampleRate = 1) {
  const liveMode = { mode: messageFlow };
  const telemetryLease = delivery.openTelemetryLease();
  return { liveMode, telemetryLease, tracer: new flow.ZLinkMessageFlowTracer({
    diagnostics: { messageFlow, sampleRate, includeMessageSizes: true },
    liveMode, sourceMeshGeneration: 1n, telemetryLease
  }) };
}

function event(packetName = 'TelemetryContract') {
  return { outcome: 'received', surface: 'channel', messageKind: 'request', packetName,
    channelName: 'api', correlationId: 'corr-1', messageSize: 19, durationSeconds: 0.125 };
}

let built = 0;
function submit(tracer, packetName) {
  const point = flow.flowIfEnabled(tracer, 'received');
  if (point) { built++; point.trace(event(packetName)); }
}

function assertNoRecordAllocation(tracer) {
  const DateConstructor = global.Date;
  let dates = 0, payloadReads = 0;
  global.Date = class extends DateConstructor {
    constructor(...args) { super(...args); dates++; }
  };
  try {
    tracer.trace({ ...event(), get packetName() { payloadReads++; return 'not-built'; } });
  } finally {
    global.Date = DateConstructor;
  }
  assert.equal(dates, 0, 'no trace timestamp allocation before provider/mode gate');
  assert.equal(payloadReads, 0, 'no snapshot payload access before provider/mode gate');
}

async function stopHost(telemetryLease) {
  let closed, lifecycle;
  await require(process.env.ZLINK_TEST_TELEMETRY_HOST_STOP).call({
    telemetryLease,
    requests: { close(error) { closed = error; } },
    runWithShutdownDeadline(_timeout, work) { return work(); },
    runLifecycle(work) { lifecycle = true; return work(); },
    async stopCore() {}
  });
  assert.equal(closed.message, 'Host stopped.');
  assert.equal(lifecycle, true);
}

(async () => {
  const commonTelemetry = mode === 'common-helper' ? require('../helpers/telemetry-log-capture') : undefined;
  const { tracer, liveMode } = makeTracer();
  const unhandled = [];
  process.on('unhandledRejection', error => unhandled.push(error));
  const threads = require('node:worker_threads');
  const Worker = threads.Worker;
  if (mode === 'worker-start-failure') {
    threads.Worker = class {
      constructor() { new Worker('invalid-worker-entry'); }
    };
  }
  let owner;
  try {
    assert.doesNotThrow(() => { owner = delivery.messageFlowDelivery(); });
  } finally {
    threads.Worker = Worker;
  }
  if (mode === 'throwing-preload' || mode === 'worker-start-failure' || mode === 'worker-exit') {
    let published = false;
    owner.ready.then(() => { published = true; });
    if (mode !== 'worker-start-failure') {
      const exited = new Promise(resolve => owner.worker.once('exit', resolve));
      if (mode === 'worker-exit') {
        await owner.ready;
        submit(tracer, 'terminating-provider-worker');
      }
      await exited;
    }
    await delay(10);
    assert.equal(unhandled.length, 0, 'worker failure does not become main unhandledRejection');
    assert.equal(owner.enabled(), false, 'dead worker cannot keep accepting trace records');
    assert.ok(owner.failure instanceof Error);
    assert.equal(Atomics.load(owner.counters, Counter.WorkerFailures), 1n);
    assert.equal(published, mode === 'worker-exit', 'failed bootstrap never publishes successful readiness');
    assert.equal(Atomics.load(owner.state, State.Pending), 0);
    assert.doesNotThrow(() => submit(tracer, 'after-worker-failure'));
    assertNoRecordAllocation(tracer);
    await stopHost(delivery.openTelemetryLease());
    console.log(`PASS ${mode}; lifecycle continues; bootstrapPublished=${published}`);
    return;
  }
  if (mode === 'bootstrap') {
    await until(() => rows().some(row => row.kind === 'bootstrap-entered'));
    let published = false;
    owner.ready.then(() => { published = true; });
    await delay(5);
    assert.equal(published, false, 'unrelated preload message is not provider readiness');
    submit(tracer, 'same-event');
    assertNoRecordAllocation(tracer);
    assert.equal(built, 0);
    assert.equal(tracer.tracedCount, 0);
    assert.equal(Atomics.load(owner.state, State.Pending), 0);
    assert.equal(Number(Atomics.load(owner.counters, Counter.Overflow)), 0);
    const closedHost = makeTracer();
    await stopHost(closedHost.telemetryLease);
    assert.equal(Atomics.load(closedHost.telemetryLease.retired, 0), 1);
    fs.writeFileSync(process.env.ZLINK_TEST_TELEMETRY_GATE, 'release');
  }
  await owner.ready;
  if (commonTelemetry) {
    submit(tracer, 'shared-helper');
    require('@opentelemetry/api-logs').logs.getLogger('test.status').emit({
      eventName: 'zlink.runtime.test', severityNumber: 9, body: 'status', attributes: { test_field: 'main' }
    });
    await commonTelemetry.observed();
    assert.equal(commonTelemetry.records.length, 2);
    assert.equal(commonTelemetry.records.find(record => record.eventId === 'zlink.runtime.test').testField, 'main');
    assert.equal(commonTelemetry.records.find(record => record.eventId === 'zlink.message_flow').packetName, 'shared-helper');
    commonTelemetry.close();
    console.log('PASS common-helper; main status and worker flow share the existing capture facade');
    return;
  } else if (mode === 'lease-renewal') {
    const lease = delivery.openTelemetryLease();
    const previousContext = flow.createDiagnosticsContext(undefined, undefined, liveMode, lease);
    await stopHost(lease);
    const renewed = delivery.renewTelemetryLease(lease);
    assert.notEqual(renewed, lease);
    assert.equal(delivery.renewTelemetryLease(renewed), renewed);
    const lateOldTracer = new flow.ZLinkMessageFlowTracer(previousContext);
    submit(lateOldTracer, 'late-old-context');
    assert.equal(built, 0);
    assert.equal(lateOldTracer.tracedCount, 0);
    const renewedContext = flow.createDiagnosticsContext(undefined, undefined, liveMode, renewed);
    submit(new flow.ZLinkMessageFlowTracer(renewedContext), 'new-host-generation');
    await until(() => !owner.executing && Atomics.load(owner.state, State.Pending) === 0);
    assert.deepEqual(rows().filter(row => row.kind === 'log').map(row => row.value.attributes.packet_name), ['new-host-generation']);
  } else if (mode === 'none' || mode === 'main-only') {
    submit(tracer);
    assertNoRecordAllocation(tracer);
    assert.equal(owner.enabled(), false);
    assert.equal(built, 0);
    assert.equal(tracer.tracedCount, 0);
    assert.equal(Atomics.load(owner.state, State.Pending), 0);
    assert.equal(Number(Atomics.load(owner.counters, Counter.Overflow)), 0);
    assert.equal(tracer.flowCreationEnabled(), true, 'provider absence does not change flow propagation');
  } else if (mode === 'registered-noop' || mode === 'logger-init-failure' || mode === 'trace-init-failure') {
    assert.equal(owner.enabled(), true);
    submit(tracer);
    await until(() => !owner.executing && Atomics.load(owner.state, State.Pending) === 0);
    assert.equal(built, 1);
    assert.equal(tracer.tracedCount, 1);
    assert.equal(rows().filter(row => row.kind === 'log' || row.kind === 'span').length, 0);
    if (mode !== 'registered-noop') assert.equal(tracer.providerFailureCount, 1);
  } else if (mode === 'blocked' || mode === 'blocked-trace' || mode === 'overflow-width') {
    let batches = 0;
    const post = owner.worker.postMessage.bind(owner.worker);
    owner.worker.postMessage = (...args) => { batches++; return post(...args); };
    const other = makeTracer();
    submit(tracer, 'blocked');
    submit(other.tracer, 'retired-in-transit');
    submit(tracer, 'survivor');
    const kind = mode === 'blocked-trace' ? 'span' : 'log';
    await until(() => rows().some(row => row.kind === kind));
    assert.equal(Atomics.load(owner.state, State.Pending), 2);
    for (let index = 0; index < 8190; index++) submit(other.tracer, 'retired-pending');
    assert.equal(Atomics.load(owner.state, State.Pending), 8192);
    // Seed the counter at the former signed-32-bit boundary, then take the real
    // admission-overflow path once. No billions-of-records stress is needed.
    if (mode === 'overflow-width') Atomics.store(owner.counters, Counter.Overflow, 2147483647n);
    const expectedOverflow = mode === 'overflow-width' ? 2147483648 : 1;
    submit(tracer, 'overflow');
    assert.equal(Number(Atomics.load(owner.counters, Counter.Overflow)), expectedOverflow);
    await delay(20);
    assert.equal(batches, 1, 'pending records do not wake the provider worker');
    const started = performance.now();
    await stopHost(other.telemetryLease);
    assert.ok(performance.now() - started < 100, 'retirement does not join blocked provider');
    submit(other.tracer, 'after-retirement');
    assert.equal(Atomics.load(owner.state, State.Pending), 2);
    fs.writeFileSync(process.env.ZLINK_TEST_TELEMETRY_GATE, 'release');
    await until(() => !owner.executing);
    assert.deepEqual(rows().filter(row => row.kind === kind).map(row => row.value.attributes.packet_name), ['blocked', 'survivor']);
    const overflow = rows().filter(row => row.kind === 'metric');
    assert.equal(overflow.length, 1);
    assert.equal(overflow[0].thread, 'worker');
    assert.deepEqual(overflow[0].value, { meterName: 'zlink.framework',
      name: 'zlink.observability.events.overflow', unit: '{event}', value: expectedOverflow,
      attributes: { source: 'message_flow' } });
  } else if (mode === 'role-counter-width') {
    submit(tracer);
    await until(() => !owner.executing && Atomics.load(owner.state, State.Pending) === 0);
    assert.equal(rows().filter(row => row.kind === 'fallback').length, 2);
    Atomics.store(owner.counters, Counter.LoggerFailures, 2147483647n);
    Atomics.store(owner.counters, Counter.TraceFailures, 4294967295n);
    for (let index = 0; index < 4; index++) submit(tracer);
    await until(() => !owner.executing && Atomics.load(owner.state, State.Pending) === 0);
    assert.equal(Atomics.load(owner.counters, Counter.LoggerFailures), 2147483651n);
    assert.equal(Atomics.load(owner.counters, Counter.TraceFailures), 4294967299n);
    assert.equal(rows().filter(row => row.kind === 'fallback').length, 2, 'counter carry does not repeat the first-failure fallback');
  } else if (mode === 'failures' || mode === 'fallback-failure') {
    for (let index = 0; index < 4; index++) submit(tracer);
    await until(() => Number(Atomics.load(owner.counters, Counter.TraceFailures)) === 4);
    assert.equal(Number(Atomics.load(owner.counters, Counter.LoggerFailures)), 4);
    assert.equal(rows().filter(row => row.kind === 'fallback').length, mode === 'failures' ? 2 : 1);
    assert.equal(Number(Atomics.load(owner.counters, Counter.FallbackFailures)), mode === 'failures' ? 0 : 1);
  } else {
    assert.equal(owner.enabled(), true, 'registered provider stays present regardless of enabled filter');
    submit(tracer, mode === 'bootstrap' ? 'same-event' : undefined);
    if (mode === 'error-filter') {
      liveMode.mode = 'errors';
      tracer.trace({ ...event(), outcome: 'error', errorReason: 'handler_exception',
        errorType: 'TypeError', errorMessage: 'handler failed', result: 'failed' });
    }
    await until(() => !owner.executing && Atomics.load(owner.state, State.Pending) === 0);
    const records = rows().filter(row => row.kind === 'log' || row.kind === 'span');
    assert.equal(records.length, mode === 'both' ? 2 : 1);
    assert.ok(records.every(row => row.thread === 'worker'));
    const attributes = records[0].value.attributes;
    assert.equal(attributes.channel_name, 'api');
    assert.equal(attributes.correlation_id, 'corr-1');
    assert.equal(attributes.message_size_bytes, undefined, 'Normal omits Detailed size');
    assert.equal(attributes.duration_seconds, undefined, 'Normal omits Detailed duration');
    assert.equal(attributes.outcome, mode === 'error-filter' ? 'failed' : 'succeeded');
    if (mode === 'error-filter') {
      assert.equal(attributes.error_type, 'TypeError');
      assert.equal(attributes.error_message, 'handler failed');
      assert.ok(rows().some(row => row.kind === 'unspecified-enabled' && row.value === false));
    }
    liveMode.mode = 'off';
    const before = built;
    submit(tracer);
    assertNoRecordAllocation(tracer);
    assert.equal(built, before);
    const sampled = makeTracer('normal', 0).tracer;
    submit(sampled);
    assert.equal(sampled.tracedCount, 0);
    liveMode.mode = 'detailed';
    submit(tracer, 'detailed');
    await until(() => !owner.executing && Atomics.load(owner.state, State.Pending) === 0);
    if (mode !== 'error-filter') {
      const detailed = rows().filter(row => row.kind === 'log' || row.kind === 'span').at(-1).value.attributes;
      assert.equal(detailed.message_size_bytes, 19);
      assert.equal(detailed.duration_seconds, 0.125);
    }
  }
  assert.equal(process.env.NODE_OPTIONS, originalNodeOptions, 'worker bootstrap does not mutate parent environment');
  if (process.env.ZLINK_TEST_TELEMETRY_ORDER === '1') {
    const ordered = thread => rows().filter(row => row.kind === 'order' && row.thread === thread).map(row => row.value);
    assert.deepEqual(ordered('worker'), ordered('main'), 'preserve standard preload order');
    assert.equal(new Set(ordered('worker')).size, 4);
    assert.equal(ordered('worker').length, 4, 'each application preload runs once');
  }
  console.log(`PASS ${mode}; builder=${built}; pending=${Atomics.load(owner.state, State.Pending)}; overflow=${Number(Atomics.load(owner.counters, Counter.Overflow))}`);
})().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.writeFileSync(process.env.ZLINK_TEST_TELEMETRY_GATE, 'release');
  clearInterval(keepAlive);
});
