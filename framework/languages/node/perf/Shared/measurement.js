'use strict';
const os = require('node:os');
const crypto = require('node:crypto');
const { monitorEventLoopDelay } = require('node:perf_hooks');
const { Histogram } = require('./histogram');
const catalog = require('../../../../perf-contract/metric-catalog.json');
const { Ranges } = require('./evidence');
const { PerfEchoRequest, PerfEchoReply, pattern, validatePattern, validateIdentity, decimal, validation } = require('./contracts');
const { scenario } = require('../Server/Scenarios/catalog');
const now = () => process.hrtime.bigint();
const unix = () => String(Date.now());
const domain = `process-${process.pid}-${crypto.randomUUID()}`;
const nullReason = (code, reason) => ({ code, reason, owner: 'perf/README.ko.md' });
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
class Measurement {
  constructor(config, primary = config.source) {
    this.config = config; this.primary = primary; this.kind = scenario(config);
    this.patterns = new Map([64, 4096].map(size => [size, pattern(size)]));
    this.publishSequence = 0n; this.phase = 'setup'; this.resetSeq = '0'; this.start = this.end = this.settled = 0n;
    this.startUnix = this.endUnix = null; this.activeHandlers = 0; this.inflight = 0n; this.maxInflight = 0n;
    this.phaseTask = Promise.resolve(); this.running = false; this.starts = new Map(); this.resetAck = null;
    this.setupEvidence = []; this.connected = 0n; this.connectionFailures = 0n;
    this.eventLoop = monitorEventLoopDelay({ resolution: 10 });
    this.clear();
  }
  clear() {
    this.intervals = new Map(Array.from({length: Math.ceil(Math.max(this.config.workload.warmupSeconds,this.config.workload.durationSeconds)*10)},(_,index)=>[index,{}])); this.counts = {}; this.directions = {}; this.byKind = {}; this.harness = {}; this.language = {}; this.errors = [];
    this.hist = new Map(); this.sealed = false; this.rssPeak = 0; this.publicSamples = []; this.cpuSamples = new Array(Math.ceil(Math.max(this.config.workload.warmupSeconds,this.config.workload.durationSeconds)*10)+2); this.cpuSampleCount=0; this.cpu=null; this.cpuStartAt=0n;
    this.attempted = new Ranges(); this.windowAdmission = new Ranges(); this.settleAdmission = new Ranges();
    this.windowDelivery = new Ranges(); this.settleDelivery = new Ranges(); this.allDelivery = new Ranges();
    this.pending = new Map(); this.terminal = new Map(); this.operations = new Map(); this.sloMet = 0n; this.sloWindowMet = 0n;
  }
  interval(key, at = now(), n = 1n) {
    if (!this.start || at < this.start || at >= this.end) return;
    const index = Number((at - this.start) / 100000000n);
    let counts = this.intervals.get(index);
    if (!counts) { counts = {}; this.intervals.set(index, counts); }
    counts[key] = (counts[key] ?? 0n) + n;
  }
  inc(map, key, n = 1n, at = now()) {
    map[key] = (map[key] ?? 0n) + n;
    if (map === this.byKind || map === this.harness || map === this.language) this.interval(`errors.${map === this.byKind ? 'byKind' : map === this.harness ? 'harness' : 'language'}.${key}`,at,n);
    if (map === this.counts) {
      this.interval(key.includes('.') ? key : key === 'lateWarmupMessages' ? 'load.lateWarmupMessages' : `messages.${key}`, at, n);
      if (key === 'sent' && !this.kind.oneWay && !this.kind.publish) this.interval('slo.eligible', at, n);
    }
  }
  timeSeries() {
    if (!this.start) return [];
    const duration = Number(this.end - this.start) / 1e6;
    return Array.from({ length: Math.ceil(duration / 100) }, (_, index) => ({
      offsetMs: index * 100, durationMs: Math.min(100, duration - index * 100),
      counts: Object.fromEntries(Object.entries(this.intervals.get(index) ?? {}).map(([key, value]) => [key, String(value)])),
      cpuPercent: this.binCpu(index), nullReasons: this.binCpu(index) === null ? { '/cpuPercent': nullReason('NO_SAMPLES', 'No actual CPU sample span starts in this bin.') } : {}
    }));
  }
  binCpu(index) {
    const samples = this.cpuSamples.slice(0,this.cpuSampleCount).filter(s => s.binIndex === index);
    return samples.length ? 100 * samples.reduce((n, s) => n + Number(s.cpuDeltaNs), 0) / samples.reduce((n, s) => n + Number(s.observedDurationNs), 0) : null;
  }
  record(key, ns) { if (key !== 'latencyMs' && key !== 'settleLatencyMs' && (!this.start || now() >= this.end)) return; let h = this.hist.get(key); if (!h) { h = new Histogram(); this.hist.set(key, h); } h.record(ns); }
  get canIssue() { return !this.sealed && this.start !== 0n && now() < this.end; }
  get hasErrors() { return Object.keys(this.byKind).length + Object.keys(this.harness).length + Object.keys(this.language).length > 0; }
  get requestBytes() { return this.kind.correlated || this.kind.oneWay || this.kind.publish ? this.config.workload.sendPayloadBytes : this.config.workload.requestPayloadBytes; }
  request(stream, sequence, probe = false) {
    const phase = probe || this.resetSeq === '0' ? 'warmup' : 'measured';
    return new PerfEchoRequest({ runId: this.config.runId, cellId: this.config.cellId, resetSeq: probe ? '0' : this.resetSeq, phase, clientId: stream, sequence: String(sequence), correlationId: `${this.config.cellId}/${phase}/${stream}/${sequence}`, sentTicks: String(now()), clockDomainId: domain, returnSpotId: null, returnChannel: null, payload: this.patterns.get(this.requestBytes) });
  }
  validate(request) {
    if (request.runId !== this.config.runId || request.cellId !== this.config.cellId || !Number.isSafeInteger(request.clientId) || request.clientId < 0 || !['warmup', 'measured'].includes(request.phase) || !request.clockDomainId || request.correlationId !== `${request.cellId}/${request.phase}/${request.clientId}/${request.sequence}`) throw validation('IdentityMismatch', 'Request identity differs.');
    decimal(request.sequence); decimal(request.sentTicks, true); decimal(request.resetSeq);
    if (request.phase === 'measured' && request.resetSeq !== this.resetSeq || request.phase === 'warmup' && request.resetSeq !== '0') throw validation('PhaseMismatch', 'Request phase/reset sequence differs.');
    validatePattern(request.payload, this.requestBytes, this.patterns.get(this.requestBytes));
    if (request.phase === 'warmup' && this.resetSeq !== '0') this.inc(this.counts, 'lateWarmupMessages');
  }
  reply(request, received = now()) {
    return new PerfEchoReply({ runId: request.runId, cellId: request.cellId, resetSeq: request.resetSeq, phase: request.phase, clientId: request.clientId, sequence: request.sequence, correlationId: request.correlationId, receivedTicks: String(received), clockDomainId: domain, payload: this.patterns.get(this.config.workload.responsePayloadBytes) });
  }
  validateReply(request, reply) { validateIdentity(request, reply); validatePattern(reply.payload, this.config.workload.responsePayloadBytes, this.patterns.get(this.config.workload.responsePayloadBytes)); }
  observeRequest(request) {
    this.validate(request);
    if (!this.setupEvidence.length) this.setupEvidence.push({ kind: 'typedProbe', source: 'public typed application handler', observedValue: { clientId: request.clientId, sequence: request.sequence } });
  }
  directional(kind, bytes) { if (this.start && now() < this.end) { this.inc(this.directions, kind); this.inc(this.directions, `${kind}Bytes`, BigInt(bytes)); this.interval(`applicationMessages.${kind}`); this.interval(`applicationPayloadBytes.${kind}`, now(), BigInt(bytes)); } }
  begin(request) {
    if (!this.canIssue) return null;
    const op = { request, started: now(), done: false, counted: !this.kind.driver };
    if (!this.kind.driver) this.inc(this.counts, 'sent'); else this.inc(this.counts, 'driver.issued');
    this.operations.set(request.correlationId, op);
    this.inflight++; if (this.inflight > this.maxInflight) this.maxInflight = this.inflight;
    if (!this.kind.driver) this.attempted.add(request.clientId, request.sequence);
    return op;
  }
  finish(op, error = null, at = now(), started = op.primaryStarted ?? op.started) {
    if (op.done || this.sealed) return false;
    op.done = true; this.inflight--; this.operations.delete(op.request.correlationId);
    if (error) this.error(error, true);
    else if (!this.kind.oneWay && !this.kind.publish) {
      this.inc(this.counts, at < this.end ? 'completed' : 'settleCompleted', 1n, at);
      this.record(at < this.end ? 'latencyMs' : 'settleLatencyMs', at - started);
      if (at - op.started <= BigInt(this.config.workload.applicationDeadlineMs) * 1000000n) { this.sloMet++; if (at < this.end) this.sloWindowMet++; }
    }
    return true;
  }
  admission(request, started) {
    const at = now(); this.inc(this.counts, 'admitted', 1n, at); this.interval(this.kind.publish ? 'messages.publishedInWindow' : 'messages.admittedInWindow', at);
    (at < this.end ? this.windowAdmission : this.settleAdmission).add(request.clientId, request.sequence);
    if (this.kind.oneWay) this.record('sendAdmissionLatencyMs', at - started);
    if (this.kind.actor) this.record('sourceAdmissionMs', at - started);
  }
  delivered(request) {
    if (request.phase === 'warmup') { if (this.resetSeq !== '0') this.inc(this.counts, 'lateWarmupMessages'); return; }
    const at = now();
    if (!this.start || at < this.start || at >= this.end + BigInt(this.config.workload.settleTimeoutMs) * 1000000n) return;
    if (this.allDelivery.add(request.clientId ?? 0, request.sequence)) { (at < this.end ? this.windowDelivery : this.settleDelivery).add(request.clientId ?? 0, request.sequence); this.interval(this.kind.publish ? 'fanout.deliveredInWindow' : 'send.deliveredInWindow', at); }
  }
  error(error, outcome = false, phaseFailure = false) {
    if (phaseFailure || this.phase === 'setup') this.inc(this.counts, 'phaseDiagnosticFailures');
    let category = 'failed';
    if (Number.isInteger(error.kind)) {
      const names = this.errorNames ?? ['NotFound', 'AlreadyExists', 'TypeMismatch', 'NotConfigured', 'Rejected', 'Unavailable', 'DeadlineExceeded', 'ShuttingDown', 'ProtocolError', 'InvalidOperation', 'DataLost', 'InternalFailure'];
      this.inc(this.byKind, names[error.kind] ?? String(error.kind));
      if (error.kind === 6) category = 'timeout';
    } else if (error.harnessKind) { this.inc(this.harness, error.harnessKind); if (error.harnessKind === 'CorrelationExpired') category = 'timeout'; }
    else { this.inc(this.language, error.name || 'Error'); if (error.name === 'AbortError') category = 'cancelled'; if (error.name === 'TimeoutError' || error.error?.code === 'requestTimeout' || error.code === 'requestTimeout') category = 'timeout'; }
    if (outcome) this.inc(this.counts, category);
    if (this.errors.length < 32) this.errors.push({ type: error.name, message: error.message, publicKind: error.kind ?? null, harnessKind: error.harnessKind ?? null });
  }
  registerCorrelation(request, op, started = op.started) {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    // A rejection can arrive while the initial public send is still awaiting admission.
    promise.catch(() => {});
    const entry = { op, request, started, resolve, reject, timer: null };
    entry.timer = setTimeout(() => { if (!this.pending.has(request.correlationId)) return; this.inc(this.counts, 'expired'); this.endCorrelation(request.correlationId, validation('CorrelationExpired', 'Application echo correlation expired.')); }, this.config.workload.correlationExpiryMs);
    this.pending.set(request.correlationId, entry);
    return promise;
  }
  endCorrelation(id, error = null, reply = null, at = now()) {
    const entry = this.pending.get(id);
    if (!entry) return false;
    this.pending.delete(id); clearTimeout(entry.timer);
    this.terminal.set(id, error ? 'failed' : 'success');
    this.finish(entry.op, error, at, entry.started);
    if (error) entry.reject(error); else entry.resolve(reply);
    return true;
  }
  receiveReply(reply) {
    if (reply.phase === 'warmup' && this.resetSeq !== '0') { this.inc(this.counts,'lateWarmupMessages'); return; }
    const entry = this.pending.get(reply.correlationId);
    if (!entry) { const previous = this.terminal.get(reply.correlationId); this.inc(this.counts, previous === 'success' ? 'duplicateReply' : previous ? 'lateReply' : 'unknownCorrelation'); return; }
    try { this.validateReply(entry.request, reply); this.endCorrelation(reply.correlationId, null, reply); }
    catch (error) { this.endCorrelation(reply.correlationId, error); }
  }
  sample() {
    const cpu = process.cpuUsage(); const at = now();
    const span = at - this.cpuLastAt; const delta = BigInt(cpu.user + cpu.system - this.cpuLast.user - this.cpuLast.system) * 1000n;
    if (span <= 0n || delta < 0n) throw validation('CpuSampleInvalid', 'CPU sampling requires positive elapsed time and nonnegative cumulative delta.');
    if (this.cpuLastAt >= this.start && this.cpuLastAt < this.end) this.cpuSamples[this.cpuSampleCount++] = { binIndex: Number((this.cpuLastAt - this.start) / 100000000n), startOffsetMs: Number(this.cpuLastAt - this.start) / 1e6, endOffsetMs: Number(at - this.start) / 1e6, observedDurationNs: String(span), cpuDeltaNs: String(delta) };
    this.cpuLast = cpu; this.cpuLastAt = at;
    this.rssPeak = Math.max(this.rssPeak, process.memoryUsage().rss); if (this.samplePublic) this.publicSamples.push({ observedTicks: String(now()), status: this.samplePublic() }); }
  startPhase(trigger, workload) {
    const ack = (accepted, state, reason = null) => ({ ...trigger, accepted, state, reason, configHash: this.config.configHash });
    if (trigger.runId !== this.config.runId || trigger.cellId !== this.config.cellId || trigger.resetSeq !== this.resetSeq || !['warmup', 'measured'].includes(trigger.phase)) return ack(false, 'rejected', 'Identity or phase mismatch.');
    const key = `${trigger.phase}/${trigger.resetSeq}`;
    if (this.starts.has(key)) return { ...this.starts.get(key), state: 'alreadyStarted' };
    if (this.running || this.inflight || this.activeHandlers || trigger.phase === 'warmup' && this.phase !== 'setup' || trigger.phase === 'measured' && this.phase !== 'reset') return ack(false, 'rejected', 'Previous phase has not drained/reset.');
    this.phase = trigger.phase; this.start = now(); this.startUnix = unix();
    this.end = this.start + BigInt(Math.round((trigger.phase === 'warmup' ? this.config.workload.warmupSeconds : this.config.workload.durationSeconds) * 1e9));
    this.cpuStart = this.cpuLast = process.cpuUsage(); this.cpuStartAt = this.cpuLastAt = now(); this.eventLoop.reset(); this.eventLoop.enable(); this.running = true;
    const answer = ack(true, 'started'); this.starts.set(key, answer);
    this.phaseTask = new Promise(resolve => setImmediate(resolve)).then(() => this.runPhase(workload)).catch(error => { this.error(error, false, true); }).finally(() => { this.running = false; });
    return answer;
  }
  async runPhase(workload) {
    const operations = workload ? workload() : Promise.resolve();
    operations.catch(error => this.error(error, false, true));
    while (now() < this.end) { this.sample(); await delay(Math.max(1, Math.min(100, Number(this.end - now()) / 1e6))); }
    this.sample(); this.cpu = {user:this.cpuLast.user-this.cpuStart.user,system:this.cpuLast.system-this.cpuStart.system}; this.eventLoop.disable(); this.endUnix = unix(); this.phase = 'settle';
    // Receiver observations have no application ACK that proves transport drain.
    // Keep the configured observation bound; active handler zero alone is insufficient.
    if (!this.primary && (this.kind.oneWay || this.kind.publish)) await delay(Math.max(0, Number(this.end + BigInt(this.config.workload.settleTimeoutMs) * 1000000n - now()) / 1e6));
    let timer;
    const settled = await Promise.race([operations.then(() => true, () => true), new Promise(resolve => { timer = setTimeout(() => resolve(false), Math.max(0, Number(this.end + BigInt(this.config.workload.settleTimeoutMs) * 1000000n - now()) / 1e6)); })]);
    clearTimeout(timer);
    if (!settled || this.inflight || this.activeHandlers) this.error(validation('SettleIncomplete', 'Application cohort did not drain by settle bound.'), false, true);
    this.counts.unresolved = this.inflight; this.settled = now(); this.sealed = true; this.phase = 'complete';
  }
  reset(request, resetCapacity = null) {
    decimal(request.resetSeq);
    if (this.resetAck?.resetSeq === request.resetSeq && !this.running && !this.inflight && !this.activeHandlers) return this.resetAck;
    let reason = null;
    if (request.runId !== this.config.runId || request.cellId !== this.config.cellId) reason = 'Different run/cell.';
    else if (BigInt(request.resetSeq) <= BigInt(this.resetSeq)) reason = 'resetSeq must advance.';
    else if (this.running || this.inflight || this.activeHandlers || this.phase !== 'complete') reason = 'Previous phase has not drained.';
    else if (Object.keys(this.harness).length || this.counts.phaseDiagnosticFailures || this.counts.lateWarmupMessages) reason = 'Previous instrumentation or phase failed.';
    const answer = { ok: !reason, ...request, role: this.config.role, roleInstance: this.config.roleInstance, applicationResetAtUnixMs: unix(), capacityEpoch: null, reason, nullReasons: {} };
    if (reason) return answer;
    this.clear(); this.resetSeq = request.resetSeq; this.phase = 'reset'; this.start = this.end = this.settled = 0n; this.startUnix = this.endUnix = null; this.maxInflight = 0n;
    if (resetCapacity) answer.capacityEpoch = String(resetCapacity()); else answer.nullReasons['/capacityEpoch'] = nullReason('NOT_APPLICABLE', 'Client has no Framework host.');
    this.resetAck = answer; return answer;
  }
  snapshot(publicStatus = null) {
    const metrics = {}, histograms = {}, nullReasons = {};
    const nil = (key, code = 'NOT_APPLICABLE', reason = 'This role/scenario has no corresponding observation.') => { metrics[key] = null; nullReasons[`/metrics/${key}`] = nullReason(code, reason); };
    const count = key => this.counts[key] ?? 0n;
    for (const key of ['sent', 'completed', 'settleCompleted', 'failed', 'timeout', 'cancelled', 'unresolved', 'admitted', 'expired', 'duplicateReply', 'lateReply', 'unknownCorrelation']) {
      if (this.primary && (!['completed', 'settleCompleted'].includes(key) || !this.kind.oneWay && !this.kind.publish) && (key !== 'admitted' || this.kind.oneWay || this.kind.correlated) && (!['expired', 'duplicateReply', 'lateReply', 'unknownCorrelation'].includes(key) || this.kind.correlated)) metrics[`messages.${key}`] = String(key === 'unresolved' && !this.sealed ? this.inflight : count(key)); else nil(`messages.${key}`);
    }
    const families = catalog.histogramPrefixes;
    for (const [key, prefix] of Object.entries(families)) {
      const emptyApplicable = this.start && (this.kind.worker && key.startsWith('worker') || this.primary && !this.kind.oneWay && !this.kind.publish && ['latencyMs', 'settleLatencyMs'].includes(key));
      const h = this.hist.get(key) ?? (emptyApplicable ? new Histogram() : null);
      if (h) h.export(key, prefix, metrics, histograms, nullReasons);
      else { const workerEmpty=this.kind.worker && key.startsWith('worker'); const unsupported = key.includes('DeliveryLatency') && (this.kind.publish || this.kind.oneWay); histograms[key] = null; nullReasons[`/histograms/${key}`] = nullReason(unsupported ? 'CLOCK_DOMAIN_UNVERIFIED' : workerEmpty ? 'NO_SAMPLES' : 'NOT_APPLICABLE', unsupported ? 'No verified cross-process delivery clock alignment.' : workerEmpty ? 'No successful worker terminal occurred inside the window.' : 'Interval not observed by this role.'); for (const suffix of catalog.latencySuffixes) nil(`${prefix}.${suffix}`, unsupported ? 'CLOCK_DOMAIN_UNVERIFIED' : 'NOT_APPLICABLE'); }
    }
    if (this.primary && this.kind.driver && !this.kind.oneWay && !this.kind.correlated) for (const suffix of catalog.latencySuffixes) { metrics[`spot.remoteCallLatency.${suffix}`] = metrics[`latency.${suffix}`]; if (metrics[`latency.${suffix}`] === null) nullReasons[`/metrics/spot.remoteCallLatency.${suffix}`] = nullReasons[`/metrics/latency.${suffix}`]; }
    else for (const suffix of catalog.latencySuffixes) nil(`spot.remoteCallLatency.${suffix}`);
    for (const key of ['spot.mailboxDepth.max', 'spot.mailboxDepth.mean', 'spot.suspendedTurns', 'spot.resumedTurns', 'spot.resumeLatency.p95Ms', 'spot.resumeLatency.p99Ms', 'worker.pool.queueDepth.max', 'worker.pool.queueDepth.mean', 'host.queueWaitLatency.p50Ms', 'host.queueWaitLatency.p95Ms', 'host.queueWaitLatency.p99Ms']) nil(key, 'PUBLIC_OBSERVATION_UNSUPPORTED', 'Exact per-owner/internal observation is not public.');
    for (const key of ['spot.applicationYieldCalls', 'spot.applicationHandlerEntries', 'driver.issued', 'driver.notStarted', 'driver.failed']) { if (this.kind.spot && (!key.startsWith('driver.') || this.kind.driver)) metrics[key] = String(count(key)); else nil(key); }
    for (const key of ['requested', 'connected', 'failed']) { if (this.config.role === 'client' && this.kind.cs) metrics[`connections.${key}`] = String(key === 'connected' ? this.connected : key === 'failed' ? this.connectionFailures : this.requestedConnections); else nil(`connections.${key}`); }
    for (const [key, value] of [['logicalStreams', this.config.workload.logicalStreams], ['inflightPerStream', this.config.workload.inflight], ['inflight.max', this.maxInflight]]) { if (this.primary && (key !== 'logicalStreams' || !this.kind.cs)) metrics[`load.${key}`] = String(value ?? 0); else nil(`load.${key}`); }
    const seconds = this.start ? Number(this.end - this.start) / 1e9 : null;
    let messages = 0n, bytes = 0n;
    for (const direction of ['request', 'send', 'reply', 'event']) { const n = this.directions[direction] ?? 0n, b = this.directions[`${direction}Bytes`] ?? 0n; metrics[`applicationMessages.${direction}`] = String(n); metrics[`applicationPayloadBytes.${direction}`] = String(b); messages += n; bytes += b; }
    if (this.primary && !this.kind.oneWay && !this.kind.publish && seconds) metrics['throughput.kops'] = Number(count('completed')) / seconds / 1000; else nil('throughput.kops', this.start ? 'NOT_APPLICABLE' : 'PHASE_NOT_STARTED');
    for (const [key, n] of [['throughput.messagesPerSec', Number(messages)], ['throughput.megabytesPerSec', Number(bytes) / 1048576]]) if (seconds) metrics[key] = n / seconds; else nil(key, 'PHASE_NOT_STARTED');
    metrics['load.lateWarmupMessages'] = String(count('lateWarmupMessages'));
    if (this.primary && !this.kind.oneWay && !this.kind.publish) { metrics['slo.eligible'] = String(count('sent')); metrics['slo.met'] = String(this.sloMet); metrics['slo.missed'] = String(count('sent') - this.sloMet); metrics['slo.missRatio'] = count('sent') ? Number(count('sent') - this.sloMet) / Number(count('sent')) : null; metrics['slo.goodputOpsPerSec'] = seconds ? Number(this.sloWindowMet) / seconds : null; }
    else for (const key of ['eligible', 'met', 'missed', 'missRatio', 'goodputOpsPerSec']) nil(`slo.${key}`, this.kind.oneWay || this.kind.publish ? 'CLOCK_DOMAIN_UNVERIFIED' : 'NOT_APPLICABLE');
    if (metrics['slo.missRatio'] === null && !nullReasons['/metrics/slo.missRatio']) nullReasons['/metrics/slo.missRatio'] = nullReason('ZERO_DENOMINATOR', 'No eligible operations.');
    if (metrics['slo.goodputOpsPerSec'] === null && !nullReasons['/metrics/slo.goodputOpsPerSec']) nullReasons['/metrics/slo.goodputOpsPerSec'] = nullReason('PHASE_NOT_STARTED', 'No measured window.');
    for (const key of ['published', 'publishedInWindow', 'settlePublished']) if (this.primary && this.kind.publish) metrics[`messages.${key}`] = String(key === 'published' ? this.windowAdmission.count() + this.settleAdmission.count() : key === 'publishedInWindow' ? this.windowAdmission.count() : this.settleAdmission.count()); else nil(`messages.${key}`);
    for (const key of ['subscriberCount', 'uniqueDelivered', 'deliveredInWindow', 'settleDelivered', 'duplicateEvents', 'outOfCohortEvents', 'deliveryRatio', 'publishOpsPerSec', 'deliveryOpsPerSec']) nil(`fanout.${key}`);
    if (this.kind.publish) metrics['fanout.subscriberCount'] = String(this.config.workload.subscriberCount);
    if (this.primary && this.kind.publish && seconds) metrics['fanout.publishOpsPerSec'] = Number(this.windowAdmission.count()) / seconds;
    for (const key of ['admissionOpsPerSec', 'deliveryOpsPerSec', 'deliveryRatio']) nil(`send.${key}`);
    if (this.primary && this.kind.oneWay && seconds) metrics['send.admissionOpsPerSec'] = Number(this.windowAdmission.count()) / seconds;
    metrics['errors.byKind'] = Object.fromEntries(Object.entries(this.byKind).map(([k, v]) => [k, String(v)])); metrics['errors.harness'] = Object.fromEntries(Object.entries(this.harness).map(([k, v]) => [k, String(v)])); metrics['errors.language'] = Object.fromEntries(Object.entries(this.language).map(([k, v]) => [k, String(v)]));
    if (this.cpu && seconds) { metrics['process.cpuPercent'] = (this.cpu.user + this.cpu.system) / (Number(this.cpuLastAt - this.cpuStartAt) / 1000) * 100; metrics['process.rssMb'] = this.rssPeak / 1048576; } else { nil('process.cpuPercent', 'PHASE_NOT_STARTED'); nil('process.rssMb', 'PHASE_NOT_STARTED'); }
    for (const key of ['process.allocatedMb', 'gc.gen0', 'gc.gen1', 'gc.gen2']) nil(key, 'PUBLIC_OBSERVATION_UNSUPPORTED', 'Node does not expose equivalent cumulative allocation/generation counters.');
    const window = { startedAtUnixMs: this.startUnix, endedAtUnixMs: this.endUnix, startTicks: this.start ? String(this.start) : null, endTicks: this.end ? String(this.end) : null, measuredSeconds: seconds, settleSeconds: this.settled ? Math.max(0, Number(this.settled - this.end) / 1e9) : null };
    for (const [key, value] of Object.entries(window)) if (value === null) nullReasons[`/window/${key}`] = nullReason('PHASE_NOT_STARTED', 'Window or settle has not completed.');
    const clock = { source: 'process.hrtime.bigint', nativeFrequencyHz: '1000000000', ticksUnit: 'ns', clockDomainId: domain, scope: 'process', alignmentMethod: null, maxErrorNs: null, validFromTicks: null, validThroughTicks: null, evidence: ['Caller RTT and worker-thread task, dispatch and continuation spans use this process monotonic clock. Remote one-way ticks require alignment.'] };
    for (const key of ['alignmentMethod', 'maxErrorNs', 'validFromTicks', 'validThroughTicks']) nullReasons[`/clock/${key}`] = nullReason('NOT_APPLICABLE', 'No cross-process clock alignment.');
    if (!publicStatus) nullReasons['/publicStatus'] = nullReason('NOT_APPLICABLE', 'Client has no Framework host runtime.');
    const serializedMessageBytes = [{ direction: this.kind.publish ? 'event' : this.kind.correlated || this.kind.oneWay ? 'send' : 'request', packetName: this.kind.publish ? 'PerfPublishEvent' : 'PerfEchoRequest', logicalPayloadBytes: String(this.requestBytes), observedSerializedBytes: null }];
    if (!this.kind.oneWay && !this.kind.publish) serializedMessageBytes.push({ direction: this.kind.correlated ? 'send' : 'reply', packetName: 'PerfEchoReply', logicalPayloadBytes: String(this.config.workload.responsePayloadBytes), observedSerializedBytes: null });
    serializedMessageBytes.forEach((_v, i) => { nullReasons[`/serializedMessageBytes/${i}/observedSerializedBytes`] = nullReason('PUBLIC_OBSERVATION_UNSUPPORTED', 'No public serialized DTO byte observation.'); });
    const streams = new Set([...this.attempted.streams.keys(), ...this.windowAdmission.streams.keys(), ...this.allDelivery.streams.keys()]);
    const runtimeMetrics = { cpuObservationSeconds: {name:'process.cpuUsage actual observation span',unit:'s',type:'number',value:this.cpuStartAt ? Number(this.cpuLastAt-this.cpuStartAt)/1e9 : 0}, cpuSamples: { name: 'process.cpuUsage actual spans, assigned by span start', unit: 'observation', type: 'array', value: this.cpuSamples.slice(0,this.cpuSampleCount) }, phaseDiagnosticFailures: { name: 'phaseDiagnosticFailures', unit: 'count', type: 'integer', value: String(count('phaseDiagnosticFailures')) }, setupEvidence: { name: 'setupEvidence', unit: 'observation', type: 'array', value: this.setupEvidence }, errors: { name: 'firstErrors', unit: 'observation', type: 'array', value: this.errors }, activeHandlers: { name: 'application active handlers', unit: 'count', type: 'integer', value: String(this.activeHandlers) }, publicReadinessSamples: { name: 'public host readiness and pressure samples', unit: 'observation', type: 'array', value: this.publicSamples }, sourceEvidence: { name: 'sourceEvidence', unit: 'observation', type: 'array', value: [...streams].sort((a, b) => a - b).map(clientId => ({ clientId, attemptedRanges: this.attempted.forStream(clientId), windowAdmissionRanges: this.windowAdmission.forStream(clientId), settleAdmissionRanges: this.settleAdmission.forStream(clientId) })) }, deliveryEvidence: { name: 'deliveryEvidence', unit: 'observation', type: 'array', value: [...this.allDelivery.streams.keys()].sort((a, b) => a - b).map(clientId => ({ clientId, windowRanges: this.windowDelivery.forStream(clientId), settleRanges: this.settleDelivery.forStream(clientId), duplicateCount: String(this.allDelivery.duplicates) })) }, nodeHeapUsedBytes: { name: 'heapUsed', unit: 'By', type: 'integer', value: String(process.memoryUsage().heapUsed) }, nodeEventLoopDelay: { name: 'eventLoopDelay', unit: 'ns', type: 'object', value: { p99: this.eventLoop.percentile(99), max: this.eventLoop.max } } };
    if (this.workerObservation) runtimeMetrics.workerObservation = { name: 'workerObservation', unit: 'observation', type: 'object', value: this.workerObservation };
    if (this.kind.publish) {
      if (this.primary) runtimeMetrics.publisherSequences = { name: 'publisherSequences', unit: 'observation', type: 'object', value: { runId: this.config.runId, cellId: this.config.cellId, resetSeq: this.resetSeq, phase: this.resetSeq === '0' ? 'warmup' : 'measured', attemptedRanges: this.attempted.forStream(0), windowSuccessRanges: this.windowAdmission.forStream(0), settleSuccessRanges: this.settleAdmission.forStream(0) } };
      else runtimeMetrics.subscriberSequences = { name: 'subscriberSequences', unit: 'observation', type: 'object', value: { runId: this.config.runId, cellId: this.config.cellId, resetSeq: this.resetSeq, phase: this.resetSeq === '0' ? 'warmup' : 'measured', subscriberId: this.config.roleInstance, windowRanges: this.windowDelivery.forStream(0), settleRanges: this.settleDelivery.forStream(0), duplicateEvents: String(this.allDelivery.duplicates), timingEvidence: null, nullReasons: { '/timingEvidence': nullReason('CLOCK_DOMAIN_UNVERIFIED', 'No verified publisher/subscriber clock domain.') } } };
    }
    for (const group of ['counts', 'rates', 'unsupported']) for (const key of catalog[group]) if (!(key in metrics)) nil(key, group === 'unsupported' ? 'PUBLIC_OBSERVATION_UNSUPPORTED' : 'NOT_APPLICABLE');
    for (const prefix of catalog.latencyPrefixes) for (const suffix of catalog.latencySuffixes) if (!(`${prefix}.${suffix}` in metrics)) nil(`${prefix}.${suffix}`);
    const snapshot = { schemaVersion: 3, runId: this.config.runId, cellId: this.config.cellId, resetSeq: this.resetSeq, language: 'node', role: this.config.role, roleInstance: this.config.roleInstance, configHash: this.config.configHash, comparisonKey: this.config.provenance.comparisonKey, phase: this.phase, window, timeSeries: this.timeSeries(), clock, serializedMessageBytes, metrics, histograms, nullReasons, publicStatus, publicMetrics: [], runtimeMetrics, provenance: { ...this.config.provenance, pid: process.pid, host: os.hostname(), configHash: this.config.configHash, resetAcknowledgement: this.resetAck, primaryEchoOwner: this.primary && !this.kind.oneWay && !this.kind.publish, runtimeVersion: process.version, effectiveProcessorCount: os.availableParallelism(), messageCountScope: 'application-call-boundaries', evidenceStorage: 'per-stream compact sequence intervals; correlation terminal states retained through report' } };
    for (const pointer of Object.keys(nullReasons)) { let value = snapshot; for (const key of pointer.slice(1).split('/')) value = value?.[key]; if (value !== null) delete nullReasons[pointer]; }
    for (const entry of runtimeMetrics.deliveryEvidence.value) entry.duplicateCount = String(this.allDelivery.duplicatesByStream.get(entry.clientId) ?? 0n);
    return snapshot;
  }
}
module.exports = { Measurement, now, unix, domain, delay };
