import fs from 'node:fs';
import path from 'node:path';
import { Injectable, Module, Scope } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkFrameworkRelocationMode,
  ZLinkMessageFlowLogMode,
  ZLinkPacket,
  type ZLinkFrameworkRelocationResult,
  type ZLinkFrameworkRuntime,
  type ZLinkRouteMeshRuntime,
  type ZLinkFanoutClient,
  type ZLinkHandlerContext,
  type ZLinkPublishContext,
  type ZLinkFanoutHandler,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
  type ZLinkSpotOutbound,
  type ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import type {
  ZLinkActorJoinRequest,
  ZLinkActorMembership,
  ZLinkMessage,
  ZLinkSpotActorJoinResult,
  ZLinkSpotTimerHandler,
  ZLinkTimer,
  ZLinkTimerTick
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_FANOUT_CLIENT,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import {
  WorkflowApplyReq,
  WorkflowProjected,
  type WorkflowApplyRes
} from '../../Shared/messages';
import {
  OBSERVABILITY_OPS_OPTIONS,
  createObservabilityOpsConfigurationModule,
  validateServerOptions,
  type ServerOptions
} from '../../configuration';
import { EvidenceStore } from '../Support/evidence-store';
import { closeHttpServer, startHttpServer } from '../Support/http-server';
import { createFlowLogRoute } from '../Support/flow-log-route';
import { MetricEvidenceCollector } from '../Support/metric-evidence-collector';

const WORKFLOW_MESH = 'observability.workflow';
const WORKFLOW_FANOUT = 'observability.projections';
const WORKFLOW_TOPIC = 'projection.updated';
let options: ServerOptions;
let evidence: EvidenceStore;
let locationStore: ZLinkRedisLocationStore;
let fanoutClient: ZLinkFanoutClient;
let stopping = false;
const metrics = new MetricEvidenceCollector();

process.once('SIGINT', () => { stopping = true; });
process.once('SIGTERM', () => { stopping = true; });

@Injectable({ scope: Scope.TRANSIENT })
class WorkflowSpot implements ZLinkSpot {
  readonly context!: ZLinkSpotContext;
  private replayed = false;
  private timer?: ZLinkTimer;

  configure(): void {
    this.context.handlers.addPacket(WorkflowApplyHandler);
  }

  async onCreate(request: ZLinkMessage): Promise<{ accepted: boolean }> {
    const initial = request.decode<WorkflowApplyReq>(Object as never);
    const existing = readState(initial.orderId);
    this.replayed = existing !== undefined;
    if (existing === undefined) writeState(initial.orderId, initial.value);
    this.timer = await this.context.addTimer('workflow-tick', 100, WorkflowTimerHandler);
    evidence.add('workflow', initial.orderId, this.replayed ? 'replayed' : 'created', String(existing ?? initial.value));
    return { accepted: true };
  }

  async onActorJoin(_actor: ZLinkActorJoinRequest, _request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult> {
    return { accepted: false };
  }

  async onJoinedActor(_actor: ZLinkActorMembership): Promise<void> {}

  async onLeaveActor(_actor: ZLinkActorMembership): Promise<void> {}

  async onDisconnectActor(_actor: ZLinkActorMembership): Promise<void> {}

  async completeTimer(): Promise<void> {
    const timer = this.timer;
    this.timer = undefined;
    await timer?.cancel();
  }

  apply(request: WorkflowApplyReq): WorkflowApplyRes {
    const value = (readState(request.orderId) ?? 0) + request.value;
    writeState(request.orderId, value);
    return { orderId: request.orderId, value, nodeRid: options.rid, replayed: this.replayed };
  }
}

@Injectable()
@ZLinkPacket('WorkflowApplyReq')
class WorkflowApplyHandler implements ZLinkSpotRequestHandler<WorkflowSpot, WorkflowApplyReq, WorkflowApplyRes> {
  async handle(spot: WorkflowSpot, request: WorkflowApplyReq, context: ZLinkHandlerContext): Promise<WorkflowApplyRes> {
    void context;
    const result = spot.apply(request);
    evidence.add('workflow', request.orderId, 'applied', `${result.value}|node=${result.nodeRid}`);
    return result;
  }
}

@Injectable()
class ProjectionHandler implements ZLinkFanoutHandler<WorkflowProjected> {
  async handle(message: WorkflowProjected, context: ZLinkPublishContext): Promise<void> {
    evidence.add('projection', message.orderId, 'received', `${message.value}|source=${message.sourceRid}|topic=${context.topic}`);
  }
}

@Injectable()
class WorkflowTimerHandler implements ZLinkSpotTimerHandler<WorkflowSpot> {
  async handle(spot: WorkflowSpot, tick: ZLinkTimerTick): Promise<void> {
    evidence.add('workflow', String(spot.context.spotId), 'timer', `tick=${tick.deliveryIndex}`);
    await fanoutClient.publish(WORKFLOW_FANOUT,
      new WorkflowProjected(String(spot.context.spotId), Number(tick.deliveryIndex), options.rid)).submit();
    await spot.completeTimer();
  }
}

class WorkflowModule {}
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
        if (options.fanoutEndpoint === undefined || options.stateFile === undefined) {
          throw new Error('Workflow requires fanoutEndpoint and stateFile.');
        }
        fs.mkdirSync(options.logDir, { recursive: true });
        evidence = new EvidenceStore(options.rid, options.evidenceFile);
        locationStore = new ZLinkRedisLocationStore({
          url: `redis://${options.redisEndpoint}`,
          keyPrefix: options.redisKeyPrefix
        });
        const builder = zlinkFramework();
        builder.addLocationStore(locationStore);
        if (options.metricsEnabled) builder.options({ metrics: { meterProvider: metrics.provider } });
        builder.configureDispatch()
          .messageFlow(options.messageFlowEnabled ? ZLinkMessageFlowLogMode.KeyTransitions : ZLinkMessageFlowLogMode.Off)
          .traceLogFile(path.join(options.logDir, `${options.rid}-flow.log`))
          .traceLabel(options.rid);
        builder.addFanoutChannel(WORKFLOW_FANOUT)
          .enablePublisher(options.fanoutEndpoint)
          .enableSubscriber()
          .addPublishHandler('WorkflowProjected', ProjectionHandler);
        const mesh = builder.addRouteMesh(WORKFLOW_MESH)
          .listen(options.routerEndpoint).routingId(options.rid);
        mesh.objects().server().addSpotFactory(
          WorkflowSpot.name,
          WorkflowSpot,
          (factory) => factory.disableRelocation()
        );
        mesh.channel(WORKFLOW_MESH).server();
        return builder.build();
      }
    })
  ],
  providers: [WorkflowSpot, WorkflowApplyHandler, ProjectionHandler, WorkflowTimerHandler]
})(WorkflowModule);

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(WorkflowModule, { logger: false, abortOnError: false });
  const spots = app.get(ZLINK_SPOT_MANAGER, { strict: false }) as ZLinkSpotManager;
  const outbound = app.get(ZLINK_SPOT_OUTBOUND, { strict: false }) as ZLinkSpotOutbound;
  const fanout = app.get(ZLINK_FANOUT_CLIENT, { strict: false }) as ZLinkFanoutClient;
  fanoutClient = fanout;
  const routeMeshRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME, { strict: false }) as ZLinkFrameworkRuntime;
  let drainResult: ZLinkFrameworkRelocationResult | undefined;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok', rid: options.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    createFlowLogRoute(options.logDir, options.rid),
    { method: 'GET', path: '/metrics', handle: () => metrics.snapshot() },
    { method: 'GET', path: '/drain/status', handle: () => ({
      ready: routeMeshRuntime.isReady(WORKFLOW_MESH), result: drainResult
    }) },
    {
      method: 'POST', path: '/workflows', handle: async (body) => {
        const request = body as WorkflowApplyReq;
        const created = await spots.getOrCreate(WorkflowSpot, request.orderId, request);
        const handle = await spots.find(String(created.spotId));
        if (handle === undefined) throw new Error(`Workflow '${request.orderId}' was not resolved.`);
        const result = await outbound.requestToSpot(handle, new WorkflowApplyReq(request.orderId, request.value))
          .timeout(5000).submit<WorkflowApplyRes>();
        await fanout.publish(WORKFLOW_FANOUT,
          new WorkflowProjected(result.orderId, result.value, result.nodeRid)).submit();
        return result;
      }
    },
    { method: 'POST', path: /^\/workflows\/([^/]+)\/close$/, handle: (_body, match) => spots.close(match![1]) },
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

function readState(orderId: string): number | undefined {
  const state = readAllState();
  return state[orderId];
}

function writeState(orderId: string, value: number): void {
  const state = readAllState();
  state[orderId] = value;
  fs.mkdirSync(path.dirname(options.stateFile!), { recursive: true });
  fs.writeFileSync(options.stateFile!, JSON.stringify(state));
}

function readAllState(): Record<string, number> {
  if (!fs.existsSync(options.stateFile!)) return {};
  return JSON.parse(fs.readFileSync(options.stateFile!, 'utf8')) as Record<string, number>;
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
