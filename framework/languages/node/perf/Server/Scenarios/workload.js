'use strict';
const { PerfDriveRequest, PerfPublishEvent, PerfProbeRequest, validation } = require('../../Shared/contracts');
const { now, delay } = require('../../Shared/measurement');
function target(ids, stream) { if (!ids.length) throw validation('IdentityMismatch', 'Configured target identity list is empty.'); return ids[stream % ids.length]; }

async function runOne(measure, clients, request) {
  const config = measure.config, kind = measure.kind;
  const op = measure.begin(request);
  if (!op) return;
  try {
    if (kind.driver) {
      const started = now();
      const result = await clients.spot.requestToSpot(target(config.spotIds, request.clientId), new PerfDriveRequest(request)).timeout(config.workload.requestTimeoutMs).submit();
      measure.record('driverLatencyMs', now() - started);
      if (!result.started) { if (!op.done) { op.done = true; measure.inflight--; measure.operations.delete(request.correlationId); } return; }
      if (kind.correlated) { const entry = measure.pending.get(request.correlationId); if (entry) await new Promise((resolve, reject) => { const yes = entry.resolve, no = entry.reject; entry.resolve = value => { yes(value); resolve(value); }; entry.reject = error => { no(error); reject(error); }; }); }
      else if (kind.oneWay) measure.finish(op);
      else { measure.validateReply(request, result.echo); measure.finish(op); }
      return;
    }
    const send = kind.correlated || kind.oneWay;
    let call;
    if (kind.publish) {
      measure.directional('event', measure.requestBytes);
      const event = new PerfPublishEvent({ runId: request.runId, cellId: request.cellId, resetSeq: request.resetSeq, phase: request.phase, sequence: request.sequence, topic: 'perf.echo', sentTicks: request.sentTicks, clockDomainId: request.clockDomainId, payload: request.payload });
      await clients.fanout.publish(config.channelName, 'perf.echo', event).submit();
      measure.admission(request, op.started); measure.finish(op); return;
    }
    if (kind.actor) call = clients.actor[send ? 'sendToActor' : 'requestToActor'](target(config.actorIds, request.clientId), request);
    else if (kind.spot) call = clients.spot[send ? 'sendToSpot' : 'requestToSpot'](target(config.spotIds, request.clientId), request);
    else call = clients.channel.requestToChannel(config.channelName, request);
    measure.directional(send ? 'send' : 'request', measure.requestBytes);
    if (send) {
      let completion;
      if (kind.correlated) { request.returnChannel = config.channelName; completion = measure.registerCorrelation(request, op); }
      try { await call.submit(); measure.admission(request, op.started); }
      catch (error) { if (kind.correlated) measure.endCorrelation(request.correlationId, error); else measure.finish(op, error); return; }
      if (kind.correlated) await completion; else measure.finish(op);
    } else {
      const reply = await call.timeout(config.workload.requestTimeoutMs).submit();
      measure.validateReply(request, reply); measure.finish(op);
    }
  } catch (error) {
    if (kind.driver) measure.inc(measure.counts, 'driver.failed');
    if (!op.done) {
      if (kind.driver && !op.counted) { op.done = true; measure.inflight--; measure.operations.delete(request.correlationId); measure.error(error); }
      else if (measure.pending.has(request.correlationId)) measure.endCorrelation(request.correlationId, error);
      else measure.finish(op, error);
    }
  }
}

async function workload(measure, clients) {
  // A logical stream retains at most the configured final-result slots. Completion
  // always yields back to the event loop; no blocking per-client thread or spin loop.
  const streams = measure.config.workload.logicalStreams;
  if (!Number.isInteger(streams) || streams <= 0) throw new Error('Server source requires logicalStreams.');
  const sequences = Array.from({ length: streams }, () => 0n);
  const lanes = [];
  // Publisher sequence is global rather than stream-local; subscribers need no ACK.
  for (let stream = 0; stream < streams; stream++) for (let slot = 0; slot < measure.config.workload.inflight; slot++) {
    lanes.push((async () => { while (measure.canIssue) { const sequence = measure.kind.publish ? ++measure.publishSequence : ++sequences[stream]; await runOne(measure, clients, measure.request(measure.kind.publish ? 0 : stream, sequence)); await new Promise(resolve => setImmediate(resolve)); } })());
  }
  await Promise.all(lanes);
}

async function prepare(measure, clients) {
  const config = measure.config, kind = measure.kind;
  if (kind.publish) {
    const request = measure.request(0, 0n, true);
    await clients.fanout.publish(config.channelName, 'perf.echo', new PerfPublishEvent({ runId: request.runId, cellId: request.cellId, resetSeq: request.resetSeq, phase: request.phase, sequence: request.sequence, topic: 'perf.echo', sentTicks: request.sentTicks, clockDomainId: request.clockDomainId, payload: request.payload })).submit();
    measure.setupEvidence.push({ kind: 'publishProbeAdmission', source: 'public fanout publish; subscriber marker evidence collected separately', observedValue: true });
    return;
  }
  const request = new PerfProbeRequest(measure.request(0, 0n, true));
  let call;
  if (kind.actor) call = clients.actor.requestToActor(target(config.actorIds, 0), request);
  else if (kind.spot) call = clients.spot.requestToSpot(target(config.spotIds, 0), request);
  else call = clients.channel.requestToChannel(config.channelName, request);
  const reply = await call.timeout(config.workload.setupTimeoutMs).submit();
  measure.validateReply(request, reply);
  if (kind.driver) {
    const remoteReply = await clients.channel.requestToChannel(config.channelName, request).timeout(config.workload.setupTimeoutMs).submit();
    measure.validateReply(request, remoteReply);
  }
  measure.setupEvidence.push({ kind: 'typedProbeEcho', source: 'public request and full typed reply validation', observedValue: { clientId: 0, sequence: '0' } });
}
module.exports = { workload, prepare, runOne, target };
