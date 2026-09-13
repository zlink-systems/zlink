'use strict';
const { PerfEchoReply, PerfDriveReply, PerfBindRequest, PerfBindReply, validation, validatePattern } = require('../Shared/contracts');
const { now, domain } = require('../Shared/measurement');

// The public CPU worker serializes its callback. Embed only validated bootstrap constants;
// the function captures no collector, payload, application state or module variable.
function cpuWork(taskMillis, clockDomainId = domain) {
  if (!Number.isInteger(taskMillis) || taskMillis <= 0) throw new RangeError('workerTaskMillis must be positive.');
  return new Function('signal', `
    const started = process.hrtime.bigint();
    let ended = started, x = 0x12345678, iterations = 0n;
    do {
      for (let i = 0; i < 1024; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; }
      iterations += 1024n;
      signal.throwIfAborted(); ended = process.hrtime.bigint();
    } while (ended - started < ${taskMillis}n * 1000000n);
    return { startedTicks: String(started), endedTicks: String(ended), clockDomainId: ${JSON.stringify(clockDomainId)}, iterations: String(iterations), checksum: x >>> 0 };
  `);
}

function createHandlers(framework, nestjs, measure, clients) {
  const config = measure.config, kind = measure.kind;
  const { Injectable } = require('@nestjs/common');
  const cpu = cpuWork(config.workload.workerTaskMillis);
  function enter(request) { measure.activeHandlers++; try { measure.observeRequest(request); } catch (error) { measure.activeHandlers--; measure.error(error); throw error; } }
  function leave() { measure.activeHandlers--; }
  function reply(request, at) { measure.directional('reply', config.workload.responsePayloadBytes); return measure.reply(request, at); }
  async function sendReply(request, outbound, at) {
    const result = measure.reply(request, at);
    measure.directional('send', config.workload.responsePayloadBytes);
    if (request.returnSpotId) await outbound.sendToSpot(request.returnSpotId, result).submit();
    else if (request.returnChannel) await outbound.sendToChannel(request.returnChannel, result).submit();
    else throw validation('IdentityMismatch', 'Send/send request lacks return address.');
  }
  class ChannelRequestHandler { async handle(request) { const at = now(); enter(request); try { return reply(request, at); } finally { leave(); } } }
  class ChannelSendHandler {
    async handle(request) { const at = now(); enter(request); try { if (kind.correlated) await sendReply(request, clients.spot, at); else measure.delivered(request); } finally { leave(); } }
  }
  class ChannelReturnHandler { async handle(result) { measure.receiveReply(result); } }
  class PerfSpot {
    async onCreate() { return { accepted: true }; }
    async onActorJoin() { return { accepted: true }; }
    async onJoinedActor() {}
    async onLeaveActor() {}
    configure() { this.context.handlers.addPacket(SpotEchoHandler); this.context.handlers.addPacket(SpotProbeHandler); if (kind.driver) this.context.handlers.addPacket(SpotDriveHandler); if (kind.correlated) this.context.handlers.addPacket(SpotReturnHandler); }
  }
  class SpotEchoHandler {
    async handle(spot, request) {
      const at = now(); enter(request); measure.inc(measure.counts, 'spot.applicationHandlerEntries');
      try {
        if (kind.worker) {
          const callStart = now(); const call = spot.context.runCpuWorker(cpu).timeoutMs(config.workload.requestTimeoutMs);
          if (config.terminal === 'yield') measure.inc(measure.counts, 'spot.applicationYieldCalls');
          const result = await call[config.terminal === 'yield' ? 'yield' : 'submit']();
          const resumed = now();
          if (!/^\d+$/.test(result.iterations) || BigInt(result.iterations) < 1024n || !Number.isInteger(result.checksum) || result.clockDomainId !== domain || BigInt(result.startedTicks) < callStart || BigInt(result.endedTicks) < BigInt(result.startedTicks) || resumed < BigInt(result.endedTicks)) throw validation('PayloadMismatch', 'Invalid worker observation.');
          measure.record('workerCallLatencyMs', resumed - callStart);
          measure.record('workerTaskLatencyMs', BigInt(result.endedTicks) - BigInt(result.startedTicks));
          measure.record('workerSubmitToStartMs', BigInt(result.startedTicks) - callStart);
          measure.record('workerResultToContinuationMs', resumed - BigInt(result.endedTicks));
          measure.workerObservation = result;
        }
        if (kind.correlated && !kind.driver) { await sendReply(request, spot.context.outbound, at); return; }
        if (kind.oneWay && !kind.driver) { measure.delivered(request); return; }
        return reply(request, at);
      } finally { leave(); }
    }
  }
  class SpotDriveHandler {
    async handle(spot, drive) {
      const request = drive.echo;
      const probe = measure.phase === 'setup' && request.phase === 'warmup';
      if (!probe && !measure.canIssue) { measure.inc(measure.counts, 'driver.notStarted'); return new PerfDriveReply(false); }
      const at = now(); enter(request); measure.inc(measure.counts, 'spot.applicationHandlerEntries');
      const op = measure.operations.get(request.correlationId);
      if (op) { op.primaryStarted = now(); if (!op.counted) { op.counted = true; measure.inc(measure.counts, 'sent'); } }
      try {
        const outbound = spot.context.outbound;
        if (kind.correlated || kind.oneWay) {
          request.returnSpotId = kind.correlated ? spot.context.spotId : null;
          if (kind.correlated && !probe) measure.registerCorrelation(request, op, op.primaryStarted);
          measure.directional('send', measure.requestBytes);
          const started = now();
          try { await outbound.sendToChannel(config.channelName, request).submit(); if (!probe) measure.admission(request, started); }
          catch (error) { if (kind.correlated && !probe) measure.endCorrelation(request.correlationId, error); throw error; }
          return new PerfDriveReply(true);
        }
        measure.directional('request', config.workload.requestPayloadBytes);
        const call = outbound.requestToChannel(config.channelName, request).timeout(config.workload.requestTimeoutMs);
        if (config.terminal === 'yield') measure.inc(measure.counts, 'spot.applicationYieldCalls');
        const result = await call[config.terminal === 'yield' ? 'yield' : 'submit']();
        measure.validateReply(request, result);
        return new PerfDriveReply(true, result);
      } finally { leave(); }
    }
  }
  class SpotReturnHandler { async handle(_spot, result) { measure.receiveReply(result); } }
  class SpotProbeHandler { async handle(_spot, request) { const at = now(); enter(request); try { return reply(request, at); } finally { leave(); } } }
  class PerfActor { constructor(context) { this.context = context; } }
  class PerfActorFactory { async create(context) { return new PerfActor(context); } }
  class PerfEntrySpot {
    async onCreateActor() { return { accepted: true }; }
    async onJoinedActor() {}
    async onLeaveActor() {}
    configure() { this.context.handlers.addHandler(ActorRequestHandler); this.context.handlers.addHandler(ActorProbeHandler); if (kind.correlated || kind.oneWay) this.context.handlers.addHandler(ActorSendHandler); }
  }
  class ActorRequestHandler { async handle(_spot, _actor, _context, request) { const at = now(); enter(request); try { return reply(request, at); } finally { leave(); } } }
  class ActorProbeHandler extends ActorRequestHandler {}
  class ActorSendHandler { async handle(_spot, _actor, _context, request) { const at = now(); enter(request); try { if (kind.correlated) await sendReply(request, clients.channel, at); else measure.delivered(request); } finally { leave(); } } }
  class PerfSessionFactory {
    async create(context) {
      return {
        context,
        async onDispatch(dispatch, payload) {
          if (dispatch.packetName === PerfBindRequest.name) {
            const bind = payload.decode(PerfBindRequest);
            if (bind.runId !== config.runId || bind.cellId !== config.cellId || !Number.isInteger(bind.clientId) || bind.clientId < 0 || bind.clientId >= config.actorIds.length) throw validation('IdentityMismatch', 'Bind identity differs.');
            const actorId = config.actorIds[bind.clientId];
            const created = await clients.actors.getOrCreate(actorId, 'perf-actor').inMesh(config.meshName).timeout(config.workload.setupTimeoutMs).submit();
            if (created.status === 'rejected') throw validation('IdentityMismatch', 'Actor preparation rejected.');
            await context.actors.bindOrGet(created.actor);
            await context.client.reply(new PerfBindReply(created.actor.actorId, true)).submit();
            if (!measure.setupEvidence.length) measure.setupEvidence.push({ kind: 'sessionBind', source: 'public manager result and session actors.bindOrGet', observedValue: actorId });
            return;
          }
          if (kind.actor) {
            const actor = context.actors.bound[0];
            if (!actor) throw validation('IdentityMismatch', 'Session is unbound.');
            measure.directional('request', config.workload.requestPayloadBytes);
            await actor.relay(dispatch, payload);
          } else {
            const request = payload.decode(require('../Shared/contracts').PerfEchoRequest); const at = now(); enter(request);
            try { await context.client.reply(reply(request, at)).submit(); } finally { leave(); }
          }
        }
      };
    }
  }
  class FanoutHandler {
    async handle(event) {
      if (event.runId !== config.runId || event.cellId !== config.cellId || event.topic !== 'perf.echo') throw validation('IdentityMismatch', 'Fanout event identity differs.');
      validatePattern(event.payload, config.workload.sendPayloadBytes, measure.patterns.get(config.workload.sendPayloadBytes));
      if (event.phase === 'measured' && event.resetSeq !== measure.resetSeq) throw validation('PhaseMismatch', 'Fanout reset sequence differs.');
      if (!measure.setupEvidence.length) measure.setupEvidence.push({ kind: 'warmupMarker', source: 'typed fanout handler', observedValue: event.sequence });
      measure.delivered(event);
    }
  }
  const providers = [ChannelRequestHandler, ChannelSendHandler, ChannelReturnHandler, PerfSpot, SpotEchoHandler, SpotProbeHandler, SpotDriveHandler, SpotReturnHandler, PerfActorFactory, PerfEntrySpot, ActorRequestHandler, ActorProbeHandler, ActorSendHandler, PerfSessionFactory, FanoutHandler];
  providers.forEach(type => Injectable()(type));
  if (kind.oneWay || kind.correlated && !kind.driver) framework.ZLinkPacket('PerfEchoRequest')(SpotEchoHandler);
  else framework.ZLinkSpotRequest('PerfEchoRequest')(SpotEchoHandler.prototype, 'handle', Object.getOwnPropertyDescriptor(SpotEchoHandler.prototype, 'handle'));
  framework.ZLinkSpotRequest('PerfProbeRequest')(SpotProbeHandler.prototype, 'handle', Object.getOwnPropertyDescriptor(SpotProbeHandler.prototype, 'handle'));
  framework.ZLinkSpotRequest('PerfDriveRequest')(SpotDriveHandler.prototype, 'handle', Object.getOwnPropertyDescriptor(SpotDriveHandler.prototype, 'handle'));
  framework.ZLinkPacket(PerfEchoReply.name)(SpotReturnHandler);
  framework.ZLinkSpotActorRequest('PerfEchoRequest')(ActorRequestHandler.prototype, 'handle', Object.getOwnPropertyDescriptor(ActorRequestHandler.prototype, 'handle'));
  framework.ZLinkSpotActorRequest('PerfProbeRequest')(ActorProbeHandler.prototype, 'handle', Object.getOwnPropertyDescriptor(ActorProbeHandler.prototype, 'handle'));
  framework.ZLinkSpotActorSend('PerfEchoRequest')(ActorSendHandler.prototype, 'handle', Object.getOwnPropertyDescriptor(ActorSendHandler.prototype, 'handle'));
  return { providers, ChannelRequestHandler, ChannelSendHandler, ChannelReturnHandler, PerfSpot, PerfActorFactory, PerfEntrySpot, PerfSessionFactory, FanoutHandler };
}
module.exports = { createHandlers, cpuWork };
