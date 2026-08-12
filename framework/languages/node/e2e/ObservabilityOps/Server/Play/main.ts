import fs from 'node:fs';
import { Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkFrameworkRelocationMode,
  ZLinkMessage,
  ZLinkSpotActorRequest,
  ZLinkSpotActorSend,
  type ActorRef,
  type ZLinkActor,
  type ZLinkActorClient,
  type ZLinkActorContext,
  type ZLinkActorFactory,
  type ZLinkActorManager,
  type ZLinkActorRelocationAdapter,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotActorRequestHandler,
  type ZLinkEntrySpotActorSendHandler,
  type ZLinkEntrySpotContext,
  type ZLinkFrameworkRelocationResult,
  type ZLinkFrameworkRuntime,
  type ZLinkMessageContext,
  type ZLinkRouteMeshRuntime,
  type ZLinkLocationRuntimeQuery,
  type ZLinkSpot,
  type ZLinkSpotActorRequestHandler,
  type ZLinkSpotActorSendHandler,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
} from '@zlink-systems/framework';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_ACTOR_MANAGER,
  ZLINK_ACTOR_CLIENT,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_SPOT_MANAGER,
  ZLinkModule,
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorRequestHandler,
  zlinkSpotActorSendHandler,
  zlinkFramework
} from '@zlink-systems/nestjs';
import {
  BoundPushNotify,
  BoundPushReq,
  HandoffProbeMsg,
  JoinTargetReq,
  ProbeReq,
  ObservabilityOpsNames,
  type ActorCreateReq,
  type ActorCreateRes,
  type ActorRefSnapshotRes,
  type BoundPushRes,
  type CreateSpotReq,
  type CreateSpotRes,
  type EvidenceWaitReq,
  type GateReleaseRes,
  type JoinTargetRes,
  type ProbeRes,
  type TransferStateDto
} from '../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';
import { closeHttpServer, startHttpServer } from '../Support/http-server';
import { createFlowLogRoute } from '../Support/flow-log-route';
import { MetricEvidenceCollector } from '../Support/metric-evidence-collector';
import { configureTelemetryLogProvider } from '../Support/telemetry-log-provider';
import {
  OBSERVABILITY_OPS_OPTIONS,
  createObservabilityOpsConfigurationModule,
  validateServerOptions
} from '../../configuration';
import type { ServerOptions } from '../../configuration';

let options: ServerOptions;
let evidence: EvidenceStore;
let transferGates: GateStore;
let locationStore: ZLinkRedisLocationStore;
let relocationStore: ZLinkRedisRelocationStore;
const metrics = new MetricEvidenceCollector();
let stopping = false;
process.once('SIGINT', () => { stopping = true; });
process.once('SIGTERM', () => { stopping = true; });
const actorScenarios = new Map<string, string>();
const capturedActorRefs = new Map<string, ActorRef>();
const actorLifecycleStates = new Map<string, { actorType: string; stateVersion: number }>();

class TransferActor implements ZLinkActor {
  actorType: string = ObservabilityOpsNames.actorTypeStateful;
  stateVersion = 0;
  readonly context!: ZLinkActorContext;

  constructor(readonly actorId: string, context?: ZLinkActorContext) {
    if (context !== undefined) Object.defineProperty(this, 'context', { value: context, configurable: true });
  }

}

@Injectable()
class TransferActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<TransferActor> {
    const actorId = context.actorId;
    actorLifecycleStates.set(actorId, { actorType: ObservabilityOpsNames.actorTypeStateful, stateVersion: 0 });
    return new TransferActor(actorId, context);
  }
}

@Injectable()
class TransferActorAdapter implements ZLinkActorRelocationAdapter<TransferActor> {
  async capture(actor: TransferActor, signal: AbortSignal): Promise<Uint8Array> {
    signal.throwIfAborted();
    evidence.add('transfer', actor.actorId, 'transfer_out', String(actor.stateVersion));
    if (actor.actorId.startsWith('actor-source-down-before-commit-')) {
      const scenario = actorScenarios.get(actor.actorId) ?? 'OBS-C1';
      evidence.add(scenario, actor.actorId, 'before_commit_gate', String(actor.stateVersion));
      await transferGates.wait(actor.actorId, signal);
    }
    actorLifecycleStates.set(actor.actorId, { actorType: actor.actorType, stateVersion: actor.stateVersion });
    return new TextEncoder().encode(JSON.stringify({
      actorId: actor.actorId,
      actorType: actor.actorType,
      stateVersion: actor.stateVersion
    } satisfies TransferStateDto));
  }

  async restore(actor: TransferActor, payload: Uint8Array, signal: AbortSignal): Promise<void> {
    signal.throwIfAborted();
    const dto = JSON.parse(new TextDecoder().decode(payload)) as TransferStateDto;
    actor.actorType = dto.actorType;
    actor.stateVersion = dto.stateVersion;
    actorLifecycleStates.set(actor.actorId, { actorType: actor.actorType, stateVersion: actor.stateVersion });
    if (actor.actorId.startsWith('actor-handoff-gate-')) {
      const scenario = actorScenarios.get(actor.actorId) ?? 'OBS-C1';
      evidence.add(scenario, actor.actorId, 'restore_gate', String(actor.stateVersion));
      await transferGates.wait(actor.actorId, signal);
    }
    evidence.add('transfer', actor.actorId, 'transfer_in', String(actor.stateVersion));
  }
}

@Injectable()
class TransferEntrySpot implements ZLinkEntrySpot<TransferActor> {
  readonly context!: ZLinkEntrySpotContext<TransferActor>;

  async onCreateActor(actor: TransferActor, request: ZLinkMessage): Promise<{ accepted: boolean }> {
    let actorType = actor.actorType;
    let stateVersion = 0;
    if (!request.toEncodedPayload().isEmpty()) {
      const create = request.decode<ActorCreateReq>(Object as never);
      actorType = create.actorType;
      stateVersion = create.stateVersion;
    }
    actor.actorType = actorType;
    actor.stateVersion = stateVersion;
    actorLifecycleStates.set(actor.actorId, { actorType, stateVersion });
    evidence.add('create', actor.actorId, 'create', `${actorType}:${stateVersion}`);
    return { accepted: true };
  }

  async onJoinedActor(actor: TransferActor): Promise<void> {
    const state = actorLifecycleStates.get(actor.actorId);
    evidence.add('local', actor.actorId, 'entry_joined', String(state?.stateVersion ?? 0));
  }

  async onLeaveActor(actor: TransferActor): Promise<void> {
    const actorId = actor.actorId;
    const state = actorLifecycleStates.get(actorId);
    evidence.add('transfer', actorId, 'leave', String(state?.stateVersion ?? 0));
    const scenario = actorScenarios.get(actorId);
    if (scenario !== undefined) {
      // Returning from this callback lets the coordinator send the commit request.
      evidence.add(scenario, actorId, 'commit_request', 'after-source-leave');
    }
  }

  async onDisconnectActor(actor: TransferActor): Promise<void> { void actor; }
}

@Injectable()
class TransferUserSpot implements ZLinkSpot<TransferActor> {
  readonly context!: ZLinkSpotContext<TransferActor>;
  private mode = 'accept';
  private readonly scenarios = new Map<string, string>();

  async onCreate(request: ZLinkMessage): Promise<{ accepted: boolean }> {
    if (!request.toEncodedPayload().isEmpty()) this.mode = request.decode<CreateSpotReq>(Object as never).mode ?? 'accept';
    evidence.add('create_spot', String(this.context.spotId), 'spot_created', this.mode);
    return { accepted: true };
  }

  async onActorJoin(actorId: string, request: ZLinkMessage): Promise<{ accepted: boolean; reply: JoinTargetRes }> {
      const join = request.decode(JoinTargetReq);
    evidence.correlate(actorId, join.transferId);
    this.scenarios.set(actorId, join.scenario);
    actorScenarios.set(actorId, join.scenario);
    evidence.add(join.scenario, actorId, 'admission', `spot=${this.context.spotId}|mode=${this.mode}|input=actor-id-only`);
    const accepted = this.mode !== 'reject' && join.expectedMode !== 'reject';
    return {
      accepted,
      reply: {
        scenario: join.scenario,
        actorId,
        accepted,
        sourceNodeRid: '',
        targetSpotId: String(this.context.spotId),
        stateVersion: 0
      }
    };
  }

  async onJoinedActor(actor: TransferActor): Promise<void> {
    const actorId = actor.actorId;
    const scenario = this.scenarios.get(actorId) ?? 'transfer';
    if (this.mode === 'fail-joined') {
      evidence.add(scenario, actorId, 'joined_failed', String(this.context.spotId));
      throw new Error('injected joined failure');
    }
    const state = actorLifecycleStates.get(actorId);
    evidence.add('transfer', actorId, 'joined', `${this.context.spotId}:${state?.stateVersion ?? 0}`);
  }

  async onLeaveActor(actor: TransferActor): Promise<void> {
    evidence.add('transfer', actor.actorId, 'target_leave', String(this.context.spotId));
  }

  async onDisconnectActor(actor: TransferActor): Promise<void> { void actor; }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: ObservabilityOpsNames.packetJoin
})
class JoinTargetHandler implements ZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, JoinTargetReq, JoinTargetRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetJoin)
  async handle(_spot: TransferEntrySpot, actor: TransferActor, _context: ZLinkMessageContext, request: JoinTargetReq): Promise<JoinTargetRes> {
    evidence.correlate(actor.actorId, request.transferId);
    actorScenarios.set(actor.actorId, request.scenario);
    actor.context.joinSpot(request.targetSpotId, request).timeout(10000).defer();
    evidence.add(request.scenario, actor.actorId, 'join_deferred', request.targetSpotId);
    return {
      scenario: request.scenario,
      actorId: actor.actorId,
      accepted: true,
      sourceNodeRid: options.rid,
      targetSpotId: request.targetSpotId,
      stateVersion: actor.stateVersion
    };
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  actor: () => TransferActor,
  spot: () => TransferUserSpot,
  packetName: ObservabilityOpsNames.packetJoin
})
class UserJoinTargetHandler implements ZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, JoinTargetReq, JoinTargetRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetJoin)
  async handle(_spot: TransferUserSpot, actor: TransferActor, _context: ZLinkMessageContext, request: JoinTargetReq): Promise<JoinTargetRes> {
    evidence.correlate(actor.actorId, request.transferId);
    actorScenarios.set(actor.actorId, request.scenario);
    actor.context.joinSpot(request.targetSpotId, request).timeout(10000).defer();
    evidence.add(request.scenario, actor.actorId, 'join_deferred', request.targetSpotId);
    return {
      scenario: request.scenario,
      actorId: actor.actorId,
      accepted: true,
      sourceNodeRid: options.rid,
      targetSpotId: request.targetSpotId,
      stateVersion: actor.stateVersion
    };
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  actor: () => TransferActor,
  spot: () => TransferUserSpot,
  packetName: ObservabilityOpsNames.packetProbe
})
class ProbeHandler implements ZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, ProbeReq, ProbeRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetProbe)
  async handle(_spot: TransferUserSpot, actor: TransferActor, _context: ZLinkMessageContext, request: ProbeReq): Promise<ProbeRes> {
    evidence.add(
      request.scenario,
      actor.actorId,
      actor.context.spotId === undefined ? 'entry_packet_handler' : 'packet_handler',
      request.marker
    );
    if (request.delayMs !== undefined) await delay(request.delayMs);
    const response = probeResponse(actorContextLocation(actor), actor, request);
    evidence.add(request.scenario, actor.actorId, 'request_reply', request.marker);
    return response;
  }
}

@Injectable()
@zlinkSpotActorSendHandler({
  actor: () => TransferActor,
  spot: () => TransferUserSpot,
  packetName: ObservabilityOpsNames.packetHandoff
})
class HandoffHandler {
  @ZLinkSpotActorSend(ObservabilityOpsNames.packetHandoff)
  async handle(_spot: TransferUserSpot, actor: TransferActor, _context: ZLinkMessageContext, message: HandoffProbeMsg): Promise<void> {
    evidence.add(
      message.scenario,
      actor.actorId,
      actor.context.spotId === undefined ? 'entry_packet_handler' : 'packet_handler',
      message.marker
    );
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: ObservabilityOpsNames.packetHandoff
})
class EntryHandoffHandler implements ZLinkEntrySpotActorSendHandler<TransferEntrySpot, TransferActor, HandoffProbeMsg> {
  @ZLinkSpotActorSend(ObservabilityOpsNames.packetHandoff)
  async handle(_spot: TransferEntrySpot, actor: TransferActor, _context: ZLinkMessageContext, message: HandoffProbeMsg): Promise<void> {
    evidence.add(message.scenario, actor.actorId, 'entry_packet_handler', message.marker);
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: ObservabilityOpsNames.packetProbe
})
class EntryProbeHandler implements ZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, ProbeReq, ProbeRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetProbe)
  async handle(_spot: TransferEntrySpot, actor: TransferActor, _context: ZLinkMessageContext, request: ProbeReq): Promise<ProbeRes> {
    evidence.add(request.scenario, actor.actorId, 'entry_packet_handler', request.marker);
    if (request.delayMs !== undefined) await delay(request.delayMs);
    const response = probeResponse(actorContextLocation(actor), actor, request);
    evidence.add(request.scenario, actor.actorId, 'request_reply', request.marker);
    return response;
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: ObservabilityOpsNames.packetBoundPush
})
class EntryBoundPushHandler implements ZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, BoundPushReq, BoundPushRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetBoundPush)
  async handle(_spot: TransferEntrySpot, actor: TransferActor, _context: ZLinkMessageContext, request: BoundPushReq): Promise<BoundPushRes> {
    return await pushBound(actorContextLocation(actor), actor, request);
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  actor: () => TransferActor,
  spot: () => TransferUserSpot,
  packetName: ObservabilityOpsNames.packetBoundPush
})
class BoundPushHandler implements ZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, BoundPushReq, BoundPushRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetBoundPush)
  async handle(_spot: TransferUserSpot, actor: TransferActor, _context: ZLinkMessageContext, request: BoundPushReq): Promise<BoundPushRes> {
    return await pushBound(actorContextLocation(actor), actor, request);
  }
}

function actorContextLocation(actor: TransferActor): { readonly spotId: unknown; readonly nodeRid: unknown } {
  return {
    spotId: actor.context.spotId ?? options.rid,
    nodeRid: options.rid
  };
}

async function pushBound(context: { spotId: unknown; nodeRid: unknown }, actor: TransferActor, request: BoundPushReq): Promise<BoundPushRes> {
  const response = probeResponse(context, actor, request);
  actor.context.boundSession.send(new BoundPushNotify(
    response.scenario,
    response.actorId,
    response.spotId,
    response.nodeRid,
    response.stateVersion,
    response.marker
  )).submit();
  evidence.add(request.scenario, actor.actorId, 'bound_push', request.marker);
  return response;
}

function probeResponse(context: { spotId: unknown; nodeRid: unknown }, actor: TransferActor, request: ProbeReq): ProbeRes {
  return {
    scenario: request.scenario,
    actorId: actor.actorId,
    spotId: String(context.spotId),
    nodeRid: String(context.nodeRid),
    stateVersion: actor.stateVersion,
    marker: request.marker
  };
}

class ActorNodeModule {}
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
        fs.mkdirSync(options.logDir, { recursive: true });
        configureTelemetryLogProvider(options.logDir, options.rid);
        evidence = new EvidenceStore(options.rid, options.evidenceFile);
        transferGates = new GateStore();
        const builder = zlinkFramework();
        locationStore = new ZLinkRedisLocationStore({
          url: `redis://${options.redisEndpoint}`,
          keyPrefix: options.redisKeyPrefix
        });
        relocationStore = new ZLinkRedisRelocationStore({
          url: `redis://${options.redisEndpoint}`,
          keyPrefix: `${options.redisKeyPrefix}:relocation`
        });
        builder.addLocationStore(locationStore);
        builder.addRelocationStore(relocationStore);
        if (options.metricsEnabled) builder.options({ metrics: { meterProvider: metrics.provider } });
        Object.assign(builder.configureLocations(), {
          pollingIntervalMs: 100,
          ownerLeaseRenewIntervalMs: 1000,
          ownerLeaseTtlMs: 5000,
          ownerLeaseFencingMarginMs: 500,
          ownerLeaseRenewTimeoutMs: 500,
          routeCacheMaxAgeMs: 500,
          messageFollowDurationMs: 6000
        });
        builder.configureDispatch()
          .messageFlow(options.messageFlowEnabled ? 'normal' : 'off');
        builder.setMessageFollowDuration(500);
        const mesh = builder.addRouteMesh(ObservabilityOpsNames.mesh)
          .listen(options.routerEndpoint).routingId(options.rid);
        const objectServer = mesh.objects().server();
        objectServer.addEntrySpot(TransferEntrySpot);
        objectServer.addSpotFactory(
          TransferUserSpot.name,
          TransferUserSpot,
          (factory) => factory.recreateOnRelocation()
        );
        objectServer.addActorFactory(
          ObservabilityOpsNames.actorTypeStateful,
          TransferActorFactory,
          (factory) => factory.preserveStateWith(TransferActorAdapter)
        );
        mesh.channel(ObservabilityOpsNames.mesh).server();
        return builder.build();
      }
    })
  ],
  providers: [
    TransferActorFactory,
    TransferActorAdapter,
    TransferEntrySpot,
    TransferUserSpot,
    JoinTargetHandler,
    UserJoinTargetHandler,
    ProbeHandler,
    HandoffHandler,
    EntryProbeHandler,
    EntryHandoffHandler,
    EntryBoundPushHandler,
    BoundPushHandler,
  ]
})(ActorNodeModule);

let actorManager: ZLinkActorManager;

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(ActorNodeModule, { logger: false, abortOnError: false });
  actorManager = app.get(ZLINK_ACTOR_MANAGER, { strict: false }) as ZLinkActorManager;
  const actorClient = app.get(ZLINK_ACTOR_CLIENT, { strict: false }) as ZLinkActorClient;
  const spots = app.get(ZLINK_SPOT_MANAGER, { strict: false }) as ZLinkSpotManager;
  const routeMeshRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME, { strict: false }) as ZLinkFrameworkRuntime;
  const locations = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  let drainResult: ZLinkFrameworkRelocationResult | undefined;
  let drainStarted = false;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok', rid: options.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'GET', path: '/location/topology', handle: () => locations.listTopology(
        {},
        { pageSize: 100 }
      )
    },
    createFlowLogRoute(options.logDir, options.rid),
    { method: 'GET', path: '/metrics', handle: () => metrics.snapshot() },
    {
      method: 'GET', path: '/drain/status', handle: async () => ({
        ready: routeMeshRuntime.isReady(ObservabilityOpsNames.mesh),
        result: drainResult === undefined
          ? undefined
          : { outcome: drainResult.outcome, reason: drainResult.reason },
        peerRows: (await locations.listTopology(
          { meshName: ObservabilityOpsNames.mesh },
          { pageSize: 100 }
        )).items.map((row) => ({
          nodeRid: String(row.nodeRid),
          draining: row.draining,
          generation: row.updatedAt.toISOString()
        })),
        actors: []
      })
    },
    {
      method: 'POST', path: '/drain', handle: (body) => {
        if (!drainStarted) {
          drainStarted = true;
          const deadlineMs = Number((body as { deadlineMs?: number }).deadlineMs ?? 30000);
          evidence.add('drain', options.rid, 'draining', `deadline=${deadlineMs}`);
          void frameworkRuntime.relocate({ mode: ZLinkFrameworkRelocationMode.PlannedMaintenance, deadlineMs }).then((result) => {
            drainResult = result;
            evidence.add('retire', options.rid, String(result.outcome), String(result.reason));
          });
        }
        return { started: true };
      }
    },
    {
      method: 'GET', path: /^\/spots\/([^/]+)\/ref$/, handle: async (_body, match) => {
        const spot = await spots.find(match![1]);
        return spot === undefined ? { found: false } : {
          found: true,
          spotId: String(spot.spotId)
        };
      }
    },
    {
      method: 'POST', path: '/evidence/wait', handle: (body) => {
        const request = body as EvidenceWaitReq;
        return evidence.waitUntil(request.containsAll, Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 40000)));
      }
    },
    {
      method: 'POST', path: /^\/transfer-gates\/([^/]+)\/release$/, handle: (_body, match) => ({
        key: match![1], released: transferGates.release(match![1])
      } satisfies GateReleaseRes)
    },
    {
      method: 'POST', path: '/spots', handle: async (body) => {
        const request = body as CreateSpotReq;
        const result = await spots
          .getOrCreate(request.spotId, TransferUserSpot.name)
          .inMesh(ObservabilityOpsNames.mesh)
          .request(request)
          .submit();
        return {
          spotId: String(result.spot.spotId),
          nodeRid: String(result.spot.nodeRid),
          state: String(result.state)
        } satisfies CreateSpotRes;
      }
    },
    {
      method: 'POST', path: /^\/spots\/([^/]+)\/close$/, handle: async (_body, match) => {
        const spot = await spots.find(match![1]);
        return { closed: spot === undefined ? false : await spots.close(spot) };
      }
    },
    {
      method: 'POST', path: '/actors', handle: async (body) => {
        const request = body as ActorCreateReq;
        const result = await actorManager
          .getOrCreate(request.actorId, request.actorType)
          .inMesh(ObservabilityOpsNames.mesh)
          .request(request)
          .submit();
        if (result.status === 'rejected') throw new Error(`Actor '${request.actorId}' creation was rejected.`);
        return {
          actorId: result.actor.actorId,
          actorType: request.actorType,
          nodeRid: String(result.actor.nodeRid),
          objectGeneration: result.actor.objectGeneration.toString(),
          meshName: result.actor.meshName
        } satisfies ActorCreateRes;
      }
    },
    {
      method: 'GET', path: /^\/actors\/([^/]+)\/ref$/, handle: async (_body, match) => {
        const actor = await requireActor(match![1]);
        capturedActorRefs.set(actor.actorId, actor);
        return actorSnapshot(actor);
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/join$/, handle: async (body, match) => {
        const actorId = match![1];
        const input = body as JoinTargetReq;
        const request = new JoinTargetReq(input.scenario, input.targetSpotId, input.expectedMode, input.transferId);
        try {
          const result = await actorClient.requestToActor(actorId, request)
            .timeout(10000).submit<JoinTargetRes>();
          evidence.add(request.scenario, actorId, result.accepted ? 'success_reply' : 'reject_reply', request.targetSpotId);
          return result;
        } catch (error) {
          const errorKind = error instanceof Error ? error.message : String(error);
          evidence.add(request.scenario, actorId, 'join_failed', errorKind);
          return { scenario: request.scenario, actorId, accepted: false, sourceNodeRid: options.rid, targetSpotId: request.targetSpotId, stateVersion: 0, errorKind } satisfies JoinTargetRes;
        }
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/probe$/, handle: async (body, match) => {
        const input = body as ProbeReq;
        const request = new ProbeReq(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs);
        return await actorClient.requestToActor(match![1], request)
          .timeout(request.requestTimeoutMs ?? 10000)
          .submit<ProbeRes>();
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/handoff$/, handle: async (body, match) => {
        const input = body as HandoffProbeMsg;
        await actorClient.sendToActor(match![1], new HandoffProbeMsg(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs)).submit();
        return { accepted: true };
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/handoff-stale$/, handle: async (body, match) => {
        const actor = capturedActorRefs.get(match![1]);
        if (actor === undefined) throw new Error(`Actor '${match![1]}' does not have a captured ref.`);
        const input = body as HandoffProbeMsg;
        await actorClient.sendToActor(match![1], new HandoffProbeMsg(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs)).submit();
        return { accepted: true };
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/probe-stale$/, handle: async (body, match) => {
        const actor = capturedActorRefs.get(match![1]);
        if (actor === undefined) throw new Error(`Actor '${match![1]}' does not have a captured ref.`);
        const input = body as ProbeReq;
        return await actorClient.requestToActor(match![1], new ProbeReq(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs))
          .timeout(10000).submit<ProbeRes>();
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/bound-push$/, handle: async (body, match) => {
        const input = body as BoundPushReq;
        return actorClient.requestToActor(match![1], new BoundPushReq(input.scenario, input.marker))
          .timeout(10000).submit<BoundPushRes>();
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);
  while (!stopping) await new Promise((resolve) => setTimeout(resolve, 100));
  await closeHttpServer(server);
  await app.close();
  await locationStore.dispose();
  await relocationStore.dispose();
}

async function requireActor(actorId: string): Promise<ActorRef> {
  const actor = await actorManager.find(actorId);
  if (actor === undefined) throw new Error(`Actor '${actorId}' was not found.`);
  return actor;
}

function actorSnapshot(actor: ActorRef): ActorRefSnapshotRes {
  return {
    actorId: actor.actorId,
    nodeRid: String(actor.nodeRid),
    objectGeneration: actor.objectGeneration.toString(),
    meshName: actor.meshName
  };
}

async function delay(milliseconds: number): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, milliseconds));
}

class GateStore {
  private readonly gates = new Map<string, { promise: Promise<void>; resolve: () => void; released: boolean }>();

  wait(key: string, signal?: AbortSignal): Promise<void> {
    let gate = this.gates.get(key);
    if (gate === undefined) {
      let resolve!: () => void;
      const promise = new Promise<void>((done) => { resolve = done; });
      gate = { promise, resolve, released: false };
      this.gates.set(key, gate);
    }
    return signal === undefined ? gate.promise : Promise.race([
      gate.promise,
      new Promise<void>((_resolve, reject) => signal.addEventListener('abort', () => reject(signal.reason), { once: true }))
    ]);
  }

  release(key: string): boolean {
    let gate = this.gates.get(key);
    if (gate === undefined) {
      let resolve!: () => void;
      const promise = new Promise<void>((done) => { resolve = done; });
      gate = { promise, resolve, released: false };
      this.gates.set(key, gate);
    }
    if (gate.released) return false;
    gate.released = true;
    gate.resolve();
    return true;
  }
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
