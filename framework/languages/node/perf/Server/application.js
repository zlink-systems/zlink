'use strict';
const http = require('node:http');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('@zlink-systems/framework');
const nestjs = require('@zlink-systems/nestjs');
const { registerPackets, json, validation } = require('../Shared/contracts');
const { Measurement, unix } = require('../Shared/measurement');
const { createHandlers } = require('./handlers');
const { workload, prepare } = require('./Scenarios/workload');
const { publicMetrics } = require('../Shared/public-metrics');
const { flowCapture } = require('../Shared/flow');

function validateConfig(config) {
  const w = config.workload;
  if (!config.runId || !config.cellId || !config.configHash) throw new Error('Role config identity required.');
  if (w.requestPayloadBytes !== 64 || w.responsePayloadBytes !== 4096 || w.sendPayloadBytes !== 4096) throw new Error('Default request/reply/send logical byte counts are 64/4096/4096.');
  for (const key of ['durationSeconds', 'warmupSeconds', 'inflight', 'requestTimeoutMs', 'settleTimeoutMs', 'setupTimeoutMs', 'applicationDeadlineMs', 'workerTaskMillis', 'workerPoolSize']) if (!(w[key] > 0)) throw new Error(`Invalid workload.${key}.`);
  if (new URL(config.metricsUrl).port === new URL(config.applicationTriggerUrl).port) throw new Error('Admin and application trigger listeners must differ.');
  if (config.store && config.store.provider !== 'redis') throw new Error('Object scenarios require the configured public Redis Location Store.');
}

async function createApplication(config) {
  validateConfig(config); registerPackets(framework);
  const measurement = new Measurement(config);
  measurement.errorNames = Object.fromEntries(Object.entries(framework.ZLinkFrameworkErrorKind).filter(([_key, value]) => typeof value === 'number').map(([key, value]) => [value, key]));
  const clients = {};
  const provider = publicMetrics();
  const flow = flowCapture(config.diagnostics);
  const h = createHandlers(framework, nestjs, measurement, clients), kind = measurement.kind;
  const objectServer = /server/i.test(config.objectRole);
  const objectClient = /client/i.test(config.objectRole);
  class PerfModule {}
  Module({ imports: [nestjs.ZLinkModule.forRootFactory({ useFactory: () => {
    const builder = nestjs.zlinkFramework().disableImplicitHandlerAutoRegistration();
    builder.options({ requestTimeoutMs: config.workload.requestTimeoutMs, metrics: { meterProvider: provider.provider }, worker: { minThreads: config.workload.workerPoolSize, maxThreads: config.workload.workerPoolSize, idleTimeoutMs: 60000 } });
    builder.configureDispatch().messageFlow(config.diagnostics ? 'normal' : 'off');
    const network = builder.configureNetwork(); network.bindHost = '127.0.0.1'; network.advertiseHost = '127.0.0.1';
    if (config.store) {
      const { ZLinkRedisLocationStore } = require('@zlink-systems/framework-locations-redis');
      builder.addLocationStore(new ZLinkRedisLocationStore({ url: `redis://${config.store.endpoint}`, keyPrefix: `${config.store.namespace}:location` }));
    }
    const meshEndpoint = kind.cs ? config.meshEndpoint : config.listenerEndpoint;
    if (!kind.publish && (meshEndpoint && config.topology !== 'clientserver' || objectServer || objectClient)) {
      if (!meshEndpoint) throw new Error('Object role requires meshEndpoint/listenerEndpoint.');
      const mesh = builder.addRouteMesh(config.meshName).listen(meshEndpoint).setPlacementWeight(objectClient && !objectServer ? 0 : 100);
      const peers = [...new Set([...(config.peerEndpoints ?? []), ...(config.peerEndpoint ? [config.peerEndpoint] : [])])];
      for (const peer of peers) if (peer !== meshEndpoint) mesh.peerConnections().connect(peer);
      if (objectServer) {
        const objects = mesh.objects().server();
        if (kind.spot) objects.addSpotFactory('perf-spot', h.PerfSpot, factory => factory.executionMode(framework.ZLinkUserSpotExecutionMode.SpotWide).disableRelocation());
        if (kind.actor) { objects.addEntrySpot(h.PerfEntrySpot); objects.addActorFactory('perf-actor', h.PerfActorFactory, factory => factory.disableRelocation()); }
      } else if (objectClient) mesh.objects().client();
      if (config.channelName) {
        const serving = !config.source || kind.correlated;
        const channel = serving ? mesh.channel(config.channelName).server() : mesh.channel(config.channelName).client();
        if (serving) {
          channel.addRequestHandler('PerfEchoRequest', h.ChannelRequestHandler).addRequestHandler('PerfProbeRequest', h.ChannelRequestHandler);
          if (kind.correlated || kind.oneWay) channel.addSendHandler('PerfEchoRequest', h.ChannelSendHandler);
          if (kind.correlated && config.source) channel.addSendHandler('PerfEchoReply', h.ChannelReturnHandler);
        }
      }
      // Public socket configuration owns the finite send wait; no readiness/retry adapter.
      mesh.configureRouterSocket().sendTimeoutMs = config.workload.socketSendTimeoutMs;
    }
    if (!kind.cs && !kind.publish && config.topology === 'clientserver') {
      const role = builder.addClientServerChannel(config.channelName);
      if (config.source) role.client().connect(config.peerEndpoint ?? config.peerEndpoints[0]);
      else role.server().listen(Number(new URL(config.listenerEndpoint).port)).setBindHost('127.0.0.1').addRequestHandler('PerfEchoRequest', h.ChannelRequestHandler).addRequestHandler('PerfProbeRequest', h.ChannelRequestHandler);
    }
    if (kind.cs && config.listenerEndpoint) {
      const stream = builder.addStreamNode('perf-session').bind(config.listenerEndpoint);
      if (kind.actor) stream.enableActorDispatch();
      stream.registerSession(h.PerfSessionFactory);
    }
    if (kind.publish) {
      const fanout = builder.addFanoutChannel(config.channelName);
      if (config.source) fanout.enablePublisher(config.fanoutEndpoint ?? config.listenerEndpoint).setRoutingIdPrefix(config.channelName);
      else fanout.enableSubscriber().addPublishHandler('PerfPublishEvent', h.FanoutHandler);
    }
    return builder.build();
  } })], providers: h.providers })(PerfModule);
  const app = await NestFactory.createApplicationContext(PerfModule, { logger: false, abortOnError: false });
  const optional = token => { try { return app.get(token, { strict: false }); } catch (error) { if (error.constructor.name === 'UnknownElementException') return null; throw error; } };
  const host = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  clients.channel = optional(nestjs.ZLINK_ROUTE_CLIENT) ?? optional(nestjs.ZLINK_CHANNEL_CLIENT);
  clients.spot = optional(nestjs.ZLINK_SPOT_OUTBOUND); clients.actor = optional(nestjs.ZLINK_ACTOR_CLIENT);
  clients.actors = optional(nestjs.ZLINK_ACTOR_MANAGER); clients.spots = optional(nestjs.ZLINK_SPOT_MANAGER);
  clients.fanout = optional(nestjs.ZLINK_FANOUT_CLIENT);
  const routeMesh = optional(nestjs.ZLINK_ROUTE_MESH_RUNTIME), clientServer = optional(nestjs.ZLINK_CLIENT_SERVER_RUNTIME), fanout = optional(nestjs.ZLINK_FANOUT_RUNTIME);
  let objectsReady = kind.cs || !objectServer || !kind.spot && !kind.actor, prepareTask = null;
  function publicStatus() {
    const status = { host: host.status };
    if (kind.publish && fanout) status.fanout = fanout.snapshot(config.channelName);
    else if (config.topology === 'clientserver' && !kind.cs && clientServer) status.clientServer = clientServer.snapshot(config.channelName);
    else if (config.meshName && routeMesh && (config.listenerEndpoint || config.meshEndpoint)) status.routeMesh = routeMesh.snapshot(config.meshName);
    return status;
  }
  function infrastructureReady() {
    const status = publicStatus();
    if (!status.host.isReady) return false;
    if (status.routeMesh && !status.routeMesh.isReady) return false;
    if (status.clientServer && !status.clientServer.isReady) return false;
    if (status.fanout && !config.source && !status.fanout.isReady) return false;
    if (config.source && !kind.local && !kind.cs && !kind.publish && status.routeMesh && kind.channel && !kind.spot && !kind.actor) {
      return status.routeMesh.channels.some(channel => channel.channelName === config.channelName && channel.isReady && channel.readyTargetCount > 0);
    }
    return true;
  }
  function ready() {
    const infra = infrastructureReady(), consumers = measurement.setupEvidence.length > 0;
    const reasons = []; if (!infra) reasons.push('Public host/topology not ready.'); if (!objectsReady) reasons.push('Public object preparation incomplete.'); if (!consumers) reasons.push('Typed probe/consumer marker evidence missing.'); if (measurement.hasErrors) reasons.push('Application failure recorded.');
    return { runId: config.runId, cellId: config.cellId, role: config.role, roleInstance: config.roleInstance, infrastructureReady: infra, objectsReady, consumersReady: consumers, ready: infra && objectsReady && consumers && !measurement.hasErrors, observedAtUnixMs: unix(), evidence: [{ kind: 'publicStatus', source: 'public Framework host and topology snapshots', observedValue: publicStatus() }, ...measurement.setupEvidence, ...measurement.errors], reasons };
  }
  measurement.samplePublic = publicStatus;
  async function prepareRole() {
    if (prepareTask) return prepareTask;
    if (config.source && !infrastructureReady()) throw validation('PhaseMismatch', 'Prepare requires public infrastructure readiness.');
    prepareTask = (async () => {
      if (objectServer && kind.spot) {
        for (const id of config.spotIds) {
          const created = await clients.spots.getOrCreate(id, 'perf-spot').inMesh(config.meshName).timeout(config.workload.setupTimeoutMs).submit();
          if (created.state === 'rejected') throw validation('IdentityMismatch', 'User Spot preparation rejected.');
          measurement.setupEvidence.push({ kind: 'userSpotReady', source: 'public Spot manager result', observedValue: created.spot });
        }
      }
      if (objectServer && kind.actor && !kind.cs) {
        for (const id of config.actorIds) {
          const created = await clients.actors.getOrCreate(id, 'perf-actor').inMesh(config.meshName).timeout(config.workload.setupTimeoutMs).submit();
          if (created.status === 'rejected') throw validation('IdentityMismatch', 'Actor preparation rejected.');
          measurement.setupEvidence.push({ kind: 'actorReady', source: 'public Actor manager result', observedValue: created.actor });
        }
      }
      objectsReady = true;
      if (config.source && !kind.cs) await prepare(measurement, clients);
      return { ok: true, ready: ready() };
    })().catch(error => { measurement.error(error); throw error; });
    return prepareTask;
  }
  // Receivers prepare their owned objects before a source's single probe. The runner
  // calls prepare on the source only after all roles report infrastructure readiness.
  if (!config.source && !kind.cs && objectServer) {
    await prepareRole();
  } else if (!objectServer) objectsReady = true;

  const servers = [];
  for (const baseUrl of [config.metricsUrl, config.applicationTriggerUrl]) {
    const url = new URL(baseUrl), admin = baseUrl === config.metricsUrl;
    const server = http.createServer(async (request, response) => {
      const pathname = new URL(request.url, baseUrl).pathname;
      const reply = (value, code = 200) => { response.writeHead(code, { 'content-type': 'application/json' }); response.end(json(value)); };
      try {
        if (admin && request.method === 'GET' && pathname === '/perf/ready') return reply(ready());
        if (admin && request.method === 'GET' && pathname === '/perf/stats') { const snapshot = measurement.snapshot(publicStatus()); snapshot.publicMetrics = await provider.snapshot(); return reply(snapshot); }
        if (request.method !== 'POST') return reply({ reason: 'Unknown endpoint.' }, 404);
        let body = ''; for await (const chunk of request) { body += chunk; if (body.length > 1048576) throw new Error('Admin request too large.'); }
        const value = body ? JSON.parse(body) : {};
        if (admin && pathname === '/perf/reset') { const ack = measurement.reset(value, () => { host.resetCapacityMetrics(); return host.status.capacity.measurementEpoch; }); return reply(ack, ack.ok ? 200 : 409); }
        if (!admin && pathname === '/app/perf/prepare') return reply(await prepareRole());
        if (!admin && pathname === '/app/perf/start') {
          if (!ready().ready) return reply({ reason: 'Preparation evidence incomplete.', ready: ready() }, 409);
          const ack = measurement.startPhase(value, config.source && !kind.cs ? () => workload(measurement, clients) : null); return reply(ack, ack.accepted ? 200 : 409);
        }
        return reply({ reason: 'Unknown endpoint.' }, 404);
      } catch (error) { reply({ reason: error.message, errorType: error.name }, error.harnessKind === 'PhaseMismatch' ? 409 : 400); }
    });
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(Number(url.port), url.hostname, resolve); }); servers.push(server);
  }
  return { app, measurement, ready, prepareRole, async close() { for (const server of servers) await new Promise(resolve => server.close(resolve)); await app.close(); await provider.close(); await flow?.shutdown(); } };
}
module.exports = { createApplication, validateConfig };
