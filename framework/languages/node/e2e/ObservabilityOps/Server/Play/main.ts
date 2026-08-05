import fs from 'node:fs';
import path from 'node:path';
import { Inject, Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkFrameworkRelocationMode,
  ZLinkMessage,
  ZLinkEncodedPayload,
  ZLinkLocationActorEventKind,
  ZLinkLocationAutoConnectType,
  ZLinkLocationRole,
  ZLinkMessageFlowLogMode,
  ZLinkSpotActorRequest,
  ZLinkSpotActorSend,
  type ActorRef,
  type ZLinkActor,
  type ZLinkActorClient,
  type ZLinkActorContext,
  type ZLinkActorFactory,
  type ZLinkActorJoinRequest,
  type ZLinkActorMembership,
  type ZLinkActorManager,
  type ZLinkActorRelocationAdapter,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotActorRequestHandler,
  type ZLinkEntrySpotActorSendHandler,
  type ZLinkEntrySpotContext,
  type ZLinkLocationActorEvent,
  type ZLinkFrameworkRelocationResult,
  type ZLinkFrameworkRuntime,
  type ZLinkRouteMeshRuntime,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRuntimeEventHandler,
  type ZLinkRuntimeEvent,
  type ZLinkSpot,
  type ZLinkSpotActorRequestContext,
  type ZLinkSpotActorSendContext,
  type ZLinkSpotActorRequestHandler,
  type ZLinkSpotActorSendHandler,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_SPOT_MANAGER,
  ZLinkModule,
  zlinkRuntimeEventHandler,
  zlinkFramework
} from '@zlink-systems/nestjs';
import {
  BoundPushNotify,
  BoundPushReq,
  HandoffProbe,
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
const metrics = new MetricEvidenceCollector();
let stopping = false;
process.once('SIGINT', () => { stopping = true; });
process.once('SIGTERM', () => { stopping = true; });
const actorScenarios = new Map<string, string>();
const capturedActorRefs = new Map<string, ActorRef>();
const actorLifecycleStates = new Map<string, { actorType: string; stateVersion: number }>();

class ApplyActorLifecycleState {
  constructor(readonly actorType: string, readonly stateVersion: number) {}
}

class ApplyActorLifecycleStateHandler {
  @ZLinkSpotActorSend('ApplyActorLifecycleState')
  async handle(
    actor: TransferActor,
    _context: ZLinkSpotActorSendContext,
    message: ApplyActorLifecycleState
  ): Promise<void> {
    actor.actorType = message.actorType;
    actor.stateVersion = message.stateVersion;
  }
}

class TransferActor implements ZLinkActor {
  actorType: string = ObservabilityOpsNames.actorTypeStateful;
  stateVersion = 0;
  readonly context!: ZLinkActorContext;

  constructor(readonly actorId: string, context?: ZLinkActorContext) {
    if (context !== undefined) Object.defineProperty(this, 'context', { value: context, configurable: true });
  }

  configure(): void {
    this.context.handlers.addHandler(JoinTargetHandler);
    this.context.handlers.addHandler(ProbeHandler);
    this.context.handlers.addHandler(HandoffHandler);
    this.context.handlers.addHandler(BoundPushHandler);
    this.context.handlers.addHandler(ApplyActorLifecycleStateHandler);
  }
}

@Injectable()
class TransferActorFactory implements ZLinkActorFactory {
  async create(actorId: string, context: ZLinkActorContext): Promise<TransferActor> {
    actorLifecycleStates.set(actorId, { actorType: ObservabilityOpsNames.actorTypeStateful, stateVersion: 0 });
    return new TransferActor(actorId, context);
  }
}

@Injectable()
class TransferActorAdapter implements ZLinkActorRelocationAdapter<TransferActor> {
  async capture(actor: TransferActor, signal: AbortSignal): Promise<Uint8Array> {
    signal.throwIfAborted();
    evidence.add('transfer', actor.actorId, 'transfer_out', String(actor.stateVersion));
    if (
      actor.actorId.startsWith('actor-source-down-before-commit-') ||
      actor.actorId.startsWith('actor-handoff-gate-')
    ) {
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
    evidence.add('transfer', actor.actorId, 'transfer_in', String(actor.stateVersion));
  }
}

@Injectable()
class TransferEntrySpot implements ZLinkEntrySpot<TransferActor> {
  readonly context!: ZLinkEntrySpotContext<TransferActor>;

  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}

  async onCreateActor(actor: ZLinkActorMembership, request: ZLinkMessage): Promise<void> {
    let actorType = actor.actorType;
    let stateVersion = 0;
    if (!request.toEncodedPayload().isEmpty()) {
      const create = request.decode<ActorCreateReq>(Object as never);
      actorType = create.actorType;
      stateVersion = create.stateVersion;
    }
    actorLifecycleStates.set(actor.actor.actorId, { actorType, stateVersion });
    await this.actors
      .sendToActor(
        ObservabilityOpsNames.mesh,
        actor.actor,
        new ApplyActorLifecycleState(actorType, stateVersion)
      )
      .submit();
    evidence.add('create', actor.actor.actorId, 'create', `${actorType}:${stateVersion}`);
  }

  async onActorJoin(actor: ZLinkActorJoinRequest, request: ZLinkMessage): Promise<{ accepted: boolean; reply?: unknown }> {
    const actorId = actor.actor.actorId;
    evidence.add('local', actorId, 'admission', 'actor-id-only');
    return { accepted: true, reply: request.decode(Object as never) };
  }

  async onJoinedActor(actor: ZLinkActorMembership): Promise<void> {
    const state = actorLifecycleStates.get(actor.actor.actorId);
    evidence.add('local', actor.actor.actorId, 'entry_joined', String(state?.stateVersion ?? 0));
  }

  async onLeaveActor(actor: ZLinkActorMembership): Promise<void> {
    const actorId = actor.actor.actorId;
    const state = actorLifecycleStates.get(actorId);
    evidence.add('transfer', actorId, 'leave', String(state?.stateVersion ?? 0));
    const scenario = actorScenarios.get(actorId);
    if (scenario !== undefined) {
      // Returning from this callback lets the coordinator send the commit request.
      evidence.add(scenario, actorId, 'commit_request', 'after-source-leave');
    }
  }

  async onDisconnectActor(actor: ZLinkActorMembership): Promise<void> { void actor; }
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

  async onActorJoin(actor: ZLinkActorJoinRequest, request: ZLinkMessage): Promise<{ accepted: boolean; reply: JoinTargetRes }> {
    const actorId = actor.actor.actorId;
    const join = request.decode<JoinTargetReq>(Object as never);
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

  async onJoinedActor(actor: ZLinkActorMembership): Promise<void> {
    const actorId = actor.actor.actorId;
    const scenario = this.scenarios.get(actorId) ?? 'transfer';
    if (this.mode === 'fail-joined') {
      evidence.add(scenario, actorId, 'joined_failed', String(this.context.spotId));
      throw new Error('injected joined failure');
    }
    const state = actorLifecycleStates.get(actorId);
    evidence.add('transfer', actorId, 'joined', `${this.context.spotId}:${state?.stateVersion ?? 0}`);
  }

  async onLeaveActor(actor: ZLinkActorMembership): Promise<void> {
    evidence.add('transfer', actor.actor.actorId, 'target_leave', String(this.context.spotId));
  }

  async onDisconnectActor(actor: ZLinkActorMembership): Promise<void> { void actor; }
}

@Injectable()
class JoinTargetHandler {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetJoin)
  async handle(actor: TransferActor, _context: ZLinkSpotActorRequestContext, request: JoinTargetReq): Promise<JoinTargetRes> {
    evidence.correlate(actor.actorId, request.transferId);
    actorScenarios.set(actor.actorId, request.scenario);
    const joined = await actor.context.joinSpot(request.targetSpotId, request).timeout(10000).submit<JoinTargetRes>();
    evidence.add(request.scenario, actor.actorId, 'commit_ack', request.targetSpotId);
    return {
      scenario: request.scenario,
      actorId: actor.actorId,
      accepted: joined.status === 'accepted',
      sourceNodeRid: options.rid,
      targetSpotId: request.targetSpotId,
      stateVersion: actor.stateVersion
    };
  }
}

@Injectable()
class UserJoinTargetHandler implements ZLinkSpotActorRequestHandler<TransferActor, JoinTargetReq, JoinTargetRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetJoin)
  async handle(actor: TransferActor, _context: ZLinkSpotActorRequestContext, request: JoinTargetReq): Promise<JoinTargetRes> {
    evidence.correlate(actor.actorId, request.transferId);
    actorScenarios.set(actor.actorId, request.scenario);
    const joined = await actor.context.joinSpot(request.targetSpotId, request).timeout(10000).submit<JoinTargetRes>();
    evidence.add(request.scenario, actor.actorId, 'commit_ack', request.targetSpotId);
    return {
      scenario: request.scenario,
      actorId: actor.actorId,
      accepted: joined.status === 'accepted',
      sourceNodeRid: options.rid,
      targetSpotId: request.targetSpotId,
      stateVersion: actor.stateVersion
    };
  }
}

@Injectable()
class ProbeHandler {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetProbe)
  async handle(actor: TransferActor, _context: ZLinkSpotActorRequestContext, request: ProbeReq): Promise<ProbeRes> {
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
class HandoffHandler {
  @ZLinkSpotActorSend(ObservabilityOpsNames.packetHandoff)
  async handle(actor: TransferActor, _context: ZLinkSpotActorSendContext, message: HandoffProbe): Promise<void> {
    evidence.add(
      message.scenario,
      actor.actorId,
      actor.context.spotId === undefined ? 'entry_packet_handler' : 'packet_handler',
      message.marker
    );
  }
}

@Injectable()
class EntryHandoffHandler implements ZLinkEntrySpotActorSendHandler<TransferActor, HandoffProbe> {
  @ZLinkSpotActorSend(ObservabilityOpsNames.packetHandoff)
  async handle(actor: TransferActor, _context: ZLinkSpotActorSendContext, message: HandoffProbe): Promise<void> {
    evidence.add(message.scenario, actor.actorId, 'entry_packet_handler', message.marker);
  }
}

@Injectable()
class EntryProbeHandler implements ZLinkEntrySpotActorRequestHandler<TransferActor, ProbeReq, ProbeRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetProbe)
  async handle(actor: TransferActor, _context: ZLinkSpotActorRequestContext, request: ProbeReq): Promise<ProbeRes> {
    evidence.add(request.scenario, actor.actorId, 'entry_packet_handler', request.marker);
    if (request.delayMs !== undefined) await delay(request.delayMs);
    const response = probeResponse(actorContextLocation(actor), actor, request);
    evidence.add(request.scenario, actor.actorId, 'request_reply', request.marker);
    return response;
  }
}

@Injectable()
class EntryBoundPushHandler implements ZLinkEntrySpotActorRequestHandler<TransferActor, BoundPushReq, BoundPushRes> {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetBoundPush)
  async handle(actor: TransferActor, _context: ZLinkSpotActorRequestContext, request: BoundPushReq): Promise<BoundPushRes> {
    return await pushBound(actorContextLocation(actor), actor, request);
  }
}

@Injectable()
class BoundPushHandler {
  @ZLinkSpotActorRequest(ObservabilityOpsNames.packetBoundPush)
  async handle(actor: TransferActor, _context: ZLinkSpotActorRequestContext, request: BoundPushReq): Promise<BoundPushRes> {
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

@Injectable()
@zlinkRuntimeEventHandler()
class ActorLocationEvidenceRecorder implements ZLinkRuntimeEventHandler<ZLinkLocationActorEvent> {
  async handle(event: ZLinkLocationActorEvent): Promise<void> {
    if (
      event.sourceName !== 'observability-ops.actor-location'
      || event.event !== ZLinkLocationActorEventKind.RowUpdated
      || event.actor?.spotId === undefined
    ) {
      return;
    }
    const scenario = actorScenarios.get(event.actor.actorId);
    if (scenario === undefined) return;
    evidence.add(
      scenario,
      event.actor.actorId,
      'location_committed',
      `node=${String(event.actor.nodeRid)}|spot=${String(event.actor.spotId)}|generation=${event.actor.generation}`
    );
  }
}

interface ActorHandoffRuntimeEvent extends ZLinkRuntimeEvent {
  readonly marker: string;
  readonly actorId: string;
  readonly index?: number;
  readonly requestSeq?: string;
  readonly flags?: number;
}

@Injectable()
@zlinkRuntimeEventHandler()
class ActorHandoffEvidenceRecorder implements ZLinkRuntimeEventHandler<ActorHandoffRuntimeEvent> {
  async handle(event: ActorHandoffRuntimeEvent): Promise<void> {
    if (event.sourceName !== 'zlink.framework.actor-handoff') return;
    const scenario = actorScenarios.get(event.actorId);
    if (scenario === undefined) return;
    const value = event.marker === 'handoff_request_frame'
      ? `index=${event.index ?? ''}|requestSeq=${event.requestSeq ?? ''}|flags=${event.flags ?? ''}`
      : event.index === undefined ? '' : String(event.index);
    evidence.add(scenario, event.actorId, event.marker, value);
  }
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
        evidence = new EvidenceStore(options.rid, options.evidenceFile);
        transferGates = new GateStore();
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
        builder.setMessageFollowDuration(500);
        const mesh = builder.addRouteMesh(ObservabilityOpsNames.mesh)
          .listen(options.routerEndpoint).routingId(options.rid);
        const objectServer = mesh.objects().server();
        objectServer.addEntrySpot(TransferEntrySpot);
        objectServer.addSpotFactory(
          TransferUserSpot.name,
          TransferUserSpot,
          (factory) => factory.disableRelocation()
        );
        objectServer.addActorFactory(
          ObservabilityOpsNames.actorTypeStateful,
          TransferActorFactory,
          (factory) => factory.preserveStateWith(TransferActorAdapter)
        );
        mesh.channel(ObservabilityOpsNames.mesh).server();
        return {
          ...builder.build(),
          monitoring: {
            locationActor: [{ sourceName: 'observability-ops.actor-location' }]
          }
        };
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
    ApplyActorLifecycleStateHandler,
    ActorLocationEvidenceRecorder,
    ActorHandoffEvidenceRecorder
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
    createFlowLogRoute(options.logDir, options.rid),
    { method: 'GET', path: '/metrics', handle: () => metrics.snapshot() },
    {
      method: 'GET', path: '/drain/status', handle: async () => ({
        ready: routeMeshRuntime.isReady(ObservabilityOpsNames.mesh),
        result: drainResult,
        peerRows: (await locations.listPeerLocations({
          autoConnectType: ZLinkLocationAutoConnectType.RouteMesh,
          meshName: ObservabilityOpsNames.mesh,
          role: ZLinkLocationRole.Spot
        })).map((row) => ({
          nodeRid: String(row.nodeRid),
          draining: row.draining,
          generation: row.generation.toString()
        })),
        actors: (await locations.listActorLocations({})).items.map((row) => ({
          actorId: row.actorId,
          nodeRid: String(row.nodeRid),
          generation: row.generation.toString()
        }))
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
        const result = await spots.getOrCreate(
          ObservabilityOpsNames.mesh,
          TransferUserSpot,
          request.spotId,
          request
        );
        return { spotId: String(result.spotId), nodeRid: options.rid, state: String(result.state) } satisfies CreateSpotRes;
      }
    },
    {
      method: 'POST', path: /^\/spots\/([^/]+)\/close$/, handle: async (_body, match) => ({
        closed: await spots.close(ObservabilityOpsNames.mesh, match![1])
      })
    },
    {
      method: 'POST', path: '/actors', handle: async (body) => {
        const request = body as ActorCreateReq;
        const actor = await actorManager.getOrCreate(
          ObservabilityOpsNames.mesh,
          request.actorId,
          request.actorType,
          request
        );
        return { actorId: actor.actorId, actorType: request.actorType, nodeRid: String(actor.nodeRid), generation: actor.generation.toString() } satisfies ActorCreateRes;
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
          const result = await actorClient.requestToActor(ObservabilityOpsNames.mesh, await requireActor(actorId), request)
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
        return await actorClient.requestToActor(ObservabilityOpsNames.mesh, await requireActor(match![1]), request)
          .timeout(request.requestTimeoutMs ?? 10000)
          .submit<ProbeRes>();
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/handoff$/, handle: async (body, match) => {
        const input = body as HandoffProbe;
        await actorClient.sendToActor(ObservabilityOpsNames.mesh, await requireActor(match![1]), new HandoffProbe(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs)).submit();
        return { accepted: true };
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/handoff-stale$/, handle: async (body, match) => {
        const actor = capturedActorRefs.get(match![1]);
        if (actor === undefined) throw new Error(`Actor '${match![1]}' does not have a captured ref.`);
        const input = body as HandoffProbe;
        await actorClient.sendToActor(ObservabilityOpsNames.mesh, actor, new HandoffProbe(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs)).submit();
        return { accepted: true };
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/probe-stale$/, handle: async (body, match) => {
        const actor = capturedActorRefs.get(match![1]);
        if (actor === undefined) throw new Error(`Actor '${match![1]}' does not have a captured ref.`);
        const input = body as ProbeReq;
        return await actorClient.requestToActor(ObservabilityOpsNames.mesh, actor, new ProbeReq(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs))
          .timeout(10000).submit<ProbeRes>();
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/bound-push$/, handle: async (body, match) => {
        const input = body as BoundPushReq;
        return actorClient.requestToActor(ObservabilityOpsNames.mesh, await requireActor(match![1]), new BoundPushReq(input.scenario, input.marker))
          .timeout(10000).submit<BoundPushRes>();
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);
  while (!stopping) await new Promise((resolve) => setTimeout(resolve, 100));
  await closeHttpServer(server);
  await app.close();
  await locationStore.dispose();
}

async function requireActor(actorId: string): Promise<ActorRef> {
  const actor = await actorManager.find(ObservabilityOpsNames.mesh, actorId);
  if (actor === undefined) throw new Error(`Actor '${actorId}' was not found.`);
  return actor;
}

function actorSnapshot(actor: ActorRef): ActorRefSnapshotRes {
  return { actorId: actor.actorId, nodeRid: String(actor.nodeRid), generation: actor.generation.toString() };
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
