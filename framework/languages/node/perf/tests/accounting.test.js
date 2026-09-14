'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { Histogram } = require('../Shared/histogram');
const { Ranges } = require('../Shared/evidence');
const { Measurement, now } = require('../Shared/measurement');
const { pattern, validatePattern, validation } = require('../Shared/contracts');
const matrix = require('../../../../perf-contract/matrix.json');
const { scenario } = require('../Server/Scenarios/catalog');
function measure(scenario = 'channel-echo-only') {
  const m = new Measurement({ runId: 'r', cellId: 'c', configHash: 'h', role: 'channel', roleInstance: 0, source: true, scenario, terminal: 'ordinary', workload: { ...matrix.defaults, logicalStreams: 2 }, provenance: { comparisonKey: 'comparison' } });
  m.start = now(); m.end = m.start + 1000000000n; return m;
}
test('the public scenario dispatcher accepts every shared default cell', () => {
  assert.equal(matrix.cells.length, 21);
  for (const cell of matrix.cells) assert.ok(scenario(cell));
});
test('canonical logical patterns distinguish 64-byte request and 4096-byte response', () => {
  validatePattern(pattern(64), 64, pattern(64)); validatePattern(pattern(4096), 4096, pattern(4096));
  assert.throws(() => validatePattern(pattern(64), 4096, pattern(4096)), { harnessKind: 'PayloadMismatch' });
});
test('shared histogram conserves counts and integer sums including overflow', () => {
  const h = new Histogram(); assert.equal(h.bounds.length, 1797);
  for (const value of [0n, 1000n, 10000n, h.bounds.at(-1) + 1n]) h.record(value);
  const s = h.snapshot(); assert.equal(s.count, '4'); assert.equal(s.overflow, '1');
  assert.equal(s.counts.reduce((n, value) => n + BigInt(value), 0n) + BigInt(s.overflow), 4n);
  assert.equal(BigInt(s.sumNs), 11001n + h.bounds.at(-1)); assert.equal(h.percentile(99), null);
});
test('delivery interval evidence merges holes and retains per-stream duplicates', () => {
  const ranges = new Ranges();
  for (const value of [5, 1, 3, 2, 4]) assert.equal(ranges.add(7, value), true);
  assert.equal(ranges.add(7, 3), false); ranges.add(8, 3);
  assert.deepEqual(ranges.forStream(7), [{ first: '1', last: '5' }]); assert.equal(ranges.count(), 6n);
  assert.equal(ranges.duplicatesByStream.get(7), 1n); assert.equal(ranges.duplicatesByStream.get(8), undefined);
});
test('timeout is a single terminal; later typed return is an additional observation', async () => {
  const m = measure('s2s-channel-to-spot-send-send-echo');
  const request = m.request(0, 1n), op = m.begin(request), pending = m.registerCorrelation(request, op);
  const error = Object.assign(new Error('deadline'), { kind: 6 }); m.endCorrelation(request.correlationId, error);
  await assert.rejects(pending, error); m.receiveReply(m.reply(request));
  assert.equal(m.counts.timeout, 1n); assert.equal(m.counts.lateReply, 1n); assert.equal(m.inflight, 0n);
  assert.equal(m.counts.completed, undefined);
});
test('admission never becomes one-way echo completion; delivery needs receiver evidence', () => {
  const m = measure('actor-no-bind-send'), request = m.request(1, 1n), op = m.begin(request);
  m.admission(request, op.started); m.finish(op);
  const snapshot = m.snapshot(); assert.equal(snapshot.schemaVersion, 3);
  assert.equal(snapshot.metrics['messages.sent'], '1'); assert.equal(snapshot.metrics['messages.admitted'], '1');
  assert.equal(snapshot.metrics['messages.completed'], null); assert.equal(snapshot.metrics['send.deliveryRatio'], null);
  assert.equal(snapshot.runtimeMetrics.sourceEvidence.value[0].windowAdmissionRanges[0].first, '1');
});
test('SLO denominator retains failures and settle successes outside the time budget', () => {
  const m = measure(); const a = m.begin(m.request(0, 1n)), b = m.begin(m.request(1, 1n));
  m.finish(a, validation('PayloadMismatch', 'bad response')); m.finish(b, null, b.started + 60000000n);
  const s = m.snapshot(); assert.equal(s.metrics['slo.eligible'], '2'); assert.equal(s.metrics['slo.met'], '0'); assert.equal(s.metrics['slo.missed'], '2'); assert.equal(s.metrics['slo.missRatio'], 1);
});

test('applicable echo histograms retain an observed zero-success cohort', () => {
  const m = measure();
  const op = m.begin(m.request(0, 1n)); m.finish(op, Object.assign(new Error('unavailable'), { kind: 0 }));
  const s = m.snapshot();
  assert.equal(s.metrics['messages.completed'], '0'); assert.equal(s.metrics['messages.settleCompleted'], '0');
  for (const [key, prefix] of [['latencyMs', 'latency'], ['settleLatencyMs', 'settle.latency']]) {
    assert.equal(s.histograms[key].count, '0'); assert.equal(s.histograms[key].sumNs, '0');
    assert.equal(s.histograms[key].maxNs, null); assert.equal(s.nullReasons[`/histograms/${key}/maxNs`].code, 'NO_SAMPLES');
    assert.equal(s.metrics[`${prefix}.meanMs`], null); assert.equal(s.nullReasons[`/metrics/${prefix}.meanMs`].code, 'NO_SAMPLES');
  }
  if (process.env.PERF_ZERO_COHORT_SNAPSHOT) require('node:fs').writeFileSync(process.env.PERF_ZERO_COHORT_SNAPSHOT, JSON.stringify(s));
});

test('schema3 emits catalog metrics and actual dense 100ms event bins', () => {
  const m = measure(); m.resetSeq = '1'; m.phase = 'measured'; m.end = m.start + 250000000n;
  const op = m.begin(m.request(0, 1n)); m.finish(op, null, m.start + 150000000n);
  const s = m.snapshot();
  assert.equal(s.comparisonKey, 'comparison');
  assert.deepEqual(s.timeSeries.map(bin => [bin.offsetMs, bin.durationMs]), [[0, 100], [100, 100], [200, 50]]);
  assert.equal(s.timeSeries[0].counts['messages.sent'], '1');
  assert.equal(s.timeSeries[1].counts['messages.completed'], '1');
  assert.equal(s.histograms.latencyMs.bucketSpec, 'ns-1us-1pct-60s-v1');
  assert.equal(s.histograms.latencyMs.boundsNs.length, 1797);
  assert.equal(s.nullReasons['/metrics/latency.p999Ms'].code, 'INSUFFICIENT_SAMPLES');
  const catalog = require('../../../../perf-contract/metric-catalog.json');
  for (const key of [...catalog.counts, ...catalog.rates]) assert.ok(key in s.metrics, key);
  if (process.env.PERF_CONTRACT_SNAPSHOT) require('node:fs').writeFileSync(process.env.PERF_CONTRACT_SNAPSHOT, JSON.stringify(s));
});

test('actual CPU spans and public language timeout warmup drain/reset preserve phases', async () => {
  const seed = measure(); const m = new Measurement({ ...seed.config, workload: { ...seed.config.workload, warmupSeconds: .01, durationSeconds: .25 } });
  const trigger = (phase, resetSeq) => ({ runId: 'r', cellId: 'c', phase, resetSeq });
  assert.equal(m.startPhase(trigger('warmup', '0'), async () => { const op=m.begin(m.request(0,1n)); const error=new Error('public timeout'); error.name='TimeoutError'; m.finish(op,error); }).accepted,true);
  await m.phaseTask;
  const warmup=m.snapshot(); assert.equal(warmup.metrics['messages.timeout'],'1'); assert.ok(warmup.metrics['errors.language'].TimeoutError);
  assert.equal(m.reset({runId:'r',cellId:'c',resetSeq:'1'}).ok,true);
  assert.equal(m.hasErrors,false);
  assert.equal(m.startPhase(trigger('measured','1'),async () => {const op=m.begin(m.request(0,1n));m.finish(op);}).accepted,true);
  await m.phaseTask;
  const snapshot=m.snapshot(); assert.equal(snapshot.phase,'complete'); assert.ok(snapshot.runtimeMetrics.cpuSamples.value.length);
  assert.equal(snapshot.metrics['slo.eligible'],'1'); assert.equal(snapshot.metrics['errors.language'].TimeoutError,undefined);
  if(process.env.PERF_CONTRACT_SNAPSHOT)require('node:fs').writeFileSync(process.env.PERF_CONTRACT_SNAPSHOT,JSON.stringify(snapshot));
});

test('one publisher sequence survives phase reset and receiver bounds exclude post-settle evidence', () => {
  const m=measure('pubsub-fanout-echo');m.publishSequence=7n;m.phase='complete';m.running=false;m.start=m.end=1n;
  assert.equal(m.reset({runId:'r',cellId:'c',resetSeq:'1'}).ok,true);assert.equal(++m.publishSequence,8n);
  m.start=1n;m.end=2n;m.delivered({phase:'measured',sequence:'9',clientId:0});assert.equal(m.allDelivery.count(),0n);
});

test('worker callback actual thread spans are recorded once by the application handler', async () => {
  const { Worker }=require('node:worker_threads');
  const { createHandlers }=require('../Server/handlers');
  const { domain }=require('../Shared/measurement');
  const seed=measure('spot-worker-offload-echo');const m=new Measurement({...seed.config,workload:{...seed.config.workload,warmupSeconds:.01,durationSeconds:.25}});
  m.startPhase({runId:'r',cellId:'c',resetSeq:'0',phase:'warmup'},async()=>{});await m.phaseTask;assert.equal(m.reset({runId:'r',cellId:'c',resetSeq:'1'}).ok,true);
  let handlerType;
  const decorators={ZLinkPacket:name=>type=>{if(name==='PerfEchoRequest')handlerType??=type;},
    ZLinkSpotRequest:name=>(target)=>{if(name==='PerfEchoRequest')handlerType??=target.constructor;},
    ZLinkSpotActorRequest:()=>()=>{},ZLinkSpotActorSend:()=>()=>{}};
  createHandlers(decorators,{},m,{});
  let calls=0;
  const context={runCpuWorker:job=>({timeoutMs:()=>({submit:()=>new Promise((resolve,reject)=>{
    calls++;const worker=new Worker(`const {parentPort,workerData}=require('node:worker_threads');parentPort.postMessage(eval('('+workerData+')')(new AbortController().signal));`,{eval:true,workerData:job.toString()});
    worker.once('message',resolve);worker.once('error',reject);
  })})})};
  let reply;
  m.startPhase({runId:'r',cellId:'c',resetSeq:'1',phase:'measured'},async()=>{const request=m.request(0,1n),op=m.begin(request);reply=await new handlerType().handle({context},request);m.finish(op);});await m.phaseTask;
  assert.equal(calls,1);assert.equal(reply.sequence,'1');assert.equal(m.workerObservation.clockDomainId,domain);
  for(const key of ['workerCallLatencyMs','workerTaskLatencyMs','workerSubmitToStartMs','workerResultToContinuationMs'])assert.equal(m.hist.get(key).count,1n,key);
  if(process.env.PERF_WORKER_SNAPSHOT)require('node:fs').writeFileSync(process.env.PERF_WORKER_SNAPSHOT,JSON.stringify(m.snapshot()));
});

test('physical and logical CCU cap is independent of in-flight requests', () => {
  const { validateCcu } = require('../Shared/contracts');
  validateCcu({ connections: 1000, logicalStreams: 1000, inflight: 2 });
  validateCcu({ connections: null, logicalStreams: 8, inflight: 1 });
  for (const key of ['connections', 'logicalStreams']) for (const value of [1001, 0, -1, 1.5, '1000']) assert.throws(() => validateCcu({ [key]: value }), /between 1 and 1000/);
});

test('independent client config entrypoint rejects CCU above 1000 before setup', () => {
  const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
  const { spawnSync } = require('node:child_process');
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-336-node-ccu-'));
  try {
    const config = path.join(folder, 'endpoints.json');
    fs.writeFileSync(config, JSON.stringify({ workload: { connections: 1001 } }));
    const result = spawnSync(process.execPath, [path.join(__dirname, '../main.js'), '--endpoint-config', config, '--client-index', '0'], { encoding: 'utf8', timeout: 5000 });
    assert.equal(result.status, 1); assert.match(result.stderr, /between 1 and 1000/);
  } finally { fs.rmSync(folder, { recursive: true }); }
});

test('canonical role preparation completes CS server Actors before source probes', async () => {
  const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
  const applicationPath = path.join(__dirname, '../Server/application.js');
  const originalRequire = require('node:module').createRequire(applicationPath);
  for (const role of ['none', 'actor', 'source']) {
    const calls = [];
    let release;
    const gate = new Promise(resolve => { release = resolve; });
    const manager = { getOrCreate(id, type) {
      assert.equal(type, 'perf-actor'); calls.push(id);
      const call = { inMesh(mesh) { assert.equal(mesh, 'mesh'); return call; },
        timeout(ms) { assert.equal(ms, matrix.defaults.setupTimeoutMs); return call; },
        async submit() { await gate; return { status: 'created', actor: { actorId: id } }; } };
      return call;
    } };
    class UnknownElementException extends Error {}
    const nestjs = { ZLINK_FRAMEWORK_RUNTIME: 'host', ZLINK_ACTOR_MANAGER: 'actors', ZLinkModule: { forRootFactory: () => ({}) } };
    const app = { get(token) { if (token === 'host') return { status: { isReady: true } }; if (token === 'actors') return manager; throw new UnknownElementException(); }, async close() {} };
    const mocks = {
      'node:http': { createServer: () => ({ once() {}, listen(_port, _host, done) { done(); }, close(done) { done(); } }) },
      '@nestjs/common': { Module: () => () => {} }, '@nestjs/core': { NestFactory: { createApplicationContext: async () => app } },
      '@zlink-systems/framework': { ZLinkFrameworkErrorKind: {} }, '@zlink-systems/nestjs': nestjs,
      '../Shared/contracts': { ...originalRequire('../Shared/contracts'), registerPackets() {} },
      './handlers': { createHandlers: () => ({ providers: [] }) },
      './Scenarios/workload': { workload() {}, async prepare() { calls.push('probe'); } },
      '../Shared/public-metrics': { publicMetrics: () => ({ async close() {} }) }, '../Shared/flow': { flowCapture: () => null }
    };
    const loaded = { exports: {} };
    vm.runInNewContext(fs.readFileSync(applicationPath, 'utf8'), { module: loaded, exports: loaded.exports, require: name => mocks[name] ?? originalRequire(name), URL, console }, { filename: applicationPath });
    const config = { runId: 'r', cellId: 'c', configHash: 'h', role: 'server', roleInstance: 0,
      source: role === 'source', scenario: role === 'source' ? 'pubsub-fanout-echo' : 'cs-remote-session-actor-echo', terminal: 'ordinary',
      objectRole: role === 'actor' ? 'serverActors' : 'None', meshName: 'mesh', actorIds: ['actor-0', 'actor-1'],
      metricsUrl: 'http://127.0.0.1:10001', applicationTriggerUrl: 'http://127.0.0.1:10002', workload: { ...matrix.defaults, logicalStreams: 2 } };
    const runtime = await loaded.exports.createApplication(config);
    try {
      assert.deepEqual(calls, [], 'startup leaves preparation to the canonical coordinator');
      assert.equal(runtime.ready().objectsReady, role !== 'actor');
      const preparing = runtime.prepareRole();
      if (role === 'actor') { assert.equal(runtime.ready().objectsReady, false); assert.deepEqual(calls, ['actor-0']); }
      release(); await preparing;
      assert.equal(runtime.ready().objectsReady, true);
      await runtime.prepareRole();
      assert.deepEqual(calls, role === 'actor' ? ['actor-0', 'actor-1'] : role === 'source' ? ['probe'] : []);
      assert.equal(runtime.measurement.setupEvidence.length, role === 'actor' ? 2 : 0);
      assert.equal(runtime.ready().consumersReady, role === 'actor');
    } finally { release(); await runtime.close(); }
  }
});
