import path from 'node:path';
import { Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkFrameworkRelocationMode,
  ZLinkMessageFlowLogMode,
  type ActorRef,
  type ZLinkFrameworkRelocationResult,
  type ZLinkFrameworkRuntime,
  type ZLinkRouteMeshRuntime,
  type ZLinkMessage,
  type ZLinkSession,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext,
  type ZLinkSessionFactory
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import { ZLINK_FRAMEWORK_RUNTIME, ZLINK_ROUTE_MESH_RUNTIME, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import {
  ObservabilityOpsNames,
  type BindActorSessionReq,
  type BindActorSessionRes
} from '../../Shared/messages';
import { closeHttpServer, startHttpServer } from '../Support/http-server';
import { createFlowLogRoute } from '../Support/flow-log-route';
import { EvidenceStore } from '../Support/evidence-store';
import { MetricEvidenceCollector } from '../Support/metric-evidence-collector';
import {
  OBSERVABILITY_OPS_OPTIONS,
  createObservabilityOpsConfigurationModule,
  validateServerOptions
} from '../../configuration';
import type { ServerOptions } from '../../configuration';

let options: ServerOptions;
let evidence: EvidenceStore;
let locationStore: ZLinkRedisLocationStore;
const metrics = new MetricEvidenceCollector();
let stopping = false;
process.once('SIGINT', () => { stopping = true; });
process.once('SIGTERM', () => { stopping = true; });

class GatewaySession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onConnected(context: ZLinkSessionContext): Promise<void> {
    evidence.add('session', context.sessionId, 'connected', context.remoteAddr ?? 'unknown');
  }

  async onDisconnected(context: ZLinkSessionContext): Promise<void> {
    evidence.add('session', context.sessionId, 'disconnected', context.remoteAddr ?? 'unknown');
  }

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage, signal?: AbortSignal): Promise<void> {
    if (dispatch.packetName === ObservabilityOpsNames.packetBindActor) {
      const request = payload.decode<BindActorSessionReq>(Object as never);
      if (request.nodeRid === undefined || request.generation === undefined) {
        throw new Error('Session gateway bind requires an ActorRef snapshot.');
      }
      const actor = {
        actorId: request.actorId,
        nodeRid: request.nodeRid,
        generation: BigInt(request.generation)
      } as ActorRef;
      evidence.correlate(request.actorId, request.transferId);
      await this.context.actors.bindOrGet(actor, signal);
      evidence.add(
        request.scenario,
        request.actorId,
        'session_bound',
        `gateway=${options.rid}|node=${String(actor.nodeRid)}|generation=${actor.generation}`
      );
      this.context.client.reply({
        scenario: request.scenario,
        actorId: actor.actorId,
        nodeRid: String(actor.nodeRid),
        generation: actor.generation.toString()
      } satisfies BindActorSessionRes).submit();
      return;
    }
    const actor = this.context.actors.bound[0];
    if (actor === undefined) throw new Error('No actor is bound.');
    await actor.relay(payload, signal);
  }
}

@Injectable()
class GatewaySessionFactory implements ZLinkSessionFactory<GatewaySession> {
  async create(context: ZLinkSessionContext): Promise<GatewaySession> { return new GatewaySession(context); }
}

class SessionModule {}
const configuration = createObservabilityOpsConfigurationModule(
  OBSERVABILITY_OPS_OPTIONS,
  validateServerOptions
);
Module({
  imports: [
    configuration,
    ZLinkModule.forRootFactory({
      imports: [configuration],
      inject: [OBSERVABILITY_OPS_OPTIONS],
      useFactory: (value: unknown) => {
        options = value as ServerOptions;
        if (options.streamEndpoint === undefined) {
          throw new Error("Configuration value 'e2e.streamEndpoint' is required for the session host.");
        }
        evidence = new EvidenceStore(options.rid, options.evidenceFile);
        const builder = zlinkFramework();
        locationStore = new ZLinkRedisLocationStore({
          url: `redis://${options.redisEndpoint}`,
          keyPrefix: options.redisKeyPrefix
        });
        builder.addLocationStore(locationStore);
        if (options.metricsEnabled) builder.options({ metrics: { meterProvider: metrics.provider } });
        Object.assign(builder.configureLocations(), {
          pollingIntervalMs: 100,
          ownerLeaseRenewIntervalMs: 1000,
          ownerLeaseTtlMs: 3000
        });
        builder.configureDispatch()
          .messageFlow(options.messageFlowEnabled ? ZLinkMessageFlowLogMode.KeyTransitions : ZLinkMessageFlowLogMode.Off)
          .traceLogFile(path.join(options.logDir, `${options.rid}-flow.log`))
          .traceLabel(options.rid);
        builder.addRouteMesh(ObservabilityOpsNames.mesh)
          .listen(options.routerEndpoint).routingId(options.rid)
          .channel(ObservabilityOpsNames.mesh).server();
        builder.addStreamNode(`${ObservabilityOpsNames.mesh}-${options.rid}`)
          .bind(options.streamEndpoint)
          .registerSession(GatewaySessionFactory);
        return builder.build();
      }
    })
  ],
  providers: [GatewaySessionFactory]
})(SessionModule);

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(SessionModule, { logger: false, abortOnError: false });
  const routeMeshRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME, { strict: false }) as ZLinkFrameworkRuntime;
  let drainResult: ZLinkFrameworkRelocationResult | undefined;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok', rid: options.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    createFlowLogRoute(options.logDir, options.rid),
    { method: 'GET', path: '/metrics', handle: () => metrics.snapshot() },
    { method: 'GET', path: '/drain/status', handle: () => ({
      ready: routeMeshRuntime.isReady(ObservabilityOpsNames.mesh), result: drainResult
    }) },
    {
      method: 'POST', path: '/drain', handle: (body) => {
        const deadlineMs = Number((body as { deadlineMs?: number }).deadlineMs ?? 30000);
        void frameworkRuntime.relocate({ mode: ZLinkFrameworkRelocationMode.PlannedMaintenance, deadlineMs })
          .then((result) => { drainResult = result; });
        return { started: true };
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);
  while (!stopping) await new Promise((resolve) => setTimeout(resolve, 100));
  await closeHttpServer(server);
  await app.close();
  await locationStore.dispose();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
