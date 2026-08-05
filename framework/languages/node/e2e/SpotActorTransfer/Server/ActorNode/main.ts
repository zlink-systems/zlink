import fs from 'node:fs';
import path from 'node:path';
import { Inject, Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessage,
  ZLinkEncodedPayload,
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkRelocationOutcome,
  ZLinkMessageFlowLogMode,
  ZLinkPeerState,
  ZLinkSpotActorRequest,
  ZLinkSpotActorSend,
  ZLinkUserSpotExecutionMode,
  type ActorRef,
  type ZLinkActor,
  type ZLinkActorClient,
  type ZLinkActorContext,
  type ZLinkActorFactory,
  type ZLinkActorJoinCompletion,
  type ZLinkActorManager,
  type ZLinkActorRelocationAdapter,
  type ZLinkFrameworkRuntime,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotActorRequestHandler,
  type ZLinkEntrySpotActorSendHandler,
  type ZLinkEntrySpotContext,
  type ZLinkMessageContext,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime,
  type ZLinkSpot,
  type ZLinkSpotActorRequestHandler,
  type ZLinkSpotActorSendHandler,
  type ZLinkSpotContext,
  type ZLinkSpotManager
} from '@zlink-systems/framework';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_MESH_RUNTIME,
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
  HandoffProbe,
  JoinTargetReq,
  ProbeReq,
  SpotActorTransferNames,
  type ActorCreateReq,
  type ActorCreateRes,
  type ActorRefRes,
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
import {
  SPOT_ACTOR_TRANSFER_OPTIONS,
  createSpotActorTransferConfigurationModule,
  validateServerOptions
} from '../../configuration';
import type { ServerOptions } from '../../configuration';
import type {
  ZLinkInternalActorTransportDeliveryGate
} from '../../../../packages/framework/dist/runtime/actors/actor-transport-delivery-gate.js';
import type {
  ZLinkActorMessageFollowContext
} from '../../../../packages/framework/dist/runtime/actors/actor-message-follow-context.js';
const { ZLINK_INTERNAL_ACTOR_TRANSPORT_DELIVERY_GATE: TRANSPORT_DELIVERY_GATE } =
  require(path.resolve(
    __dirname,
    '../../../../../../../packages/framework/dist/runtime/actors/actor-transport-delivery-gate.js'
  )) as typeof import(
    '../../../../packages/framework/dist/runtime/actors/actor-transport-delivery-gate.js'
  );

let options: ServerOptions;
let evidence: EvidenceStore;
let domainState: DomainStateStore;
let joinedGates: GateStore;
let transferGates: GateStore;
let completionGates: GateStore;
let stopping = false;
process.once('SIGINT', () => { stopping = true; });
process.once('SIGTERM', () => { stopping = true; });
const actorScenarios = new Map<string, string>();
const actorLifecycleStates = new Map<string, { actorType: string; stateVersion: number }>();
const joinCompletions = new Map<string, ZLinkActorJoinCompletion>();

@Injectable()
class TransportDeliveryGate implements ZLinkInternalActorTransportDeliveryGate {
  private readonly entries = new Map<string, {
    readonly actorId: string;
    readonly kind: 'oneWay' | 'request';
    readonly captured: Promise<void>;
    readonly markCaptured: () => void;
    readonly released: Promise<number>;
    readonly release: (submissionCopies: number) => void;
    capturedCount: number;
    releasedCount: number;
  }>();

  arm(operationId: string, actorId: string, kind: 'oneWay' | 'request'): void {
    let markCaptured!: () => void;
    let release!: (submissionCopies: number) => void;
    this.entries.set(operationId, {
      actorId,
      kind,
      captured: new Promise<void>((resolve) => { markCaptured = resolve; }),
      markCaptured,
      released: new Promise<number>((resolve) => { release = resolve; }),
      release,
      capturedCount: 0,
      releasedCount: 0
    });
  }

  async waitBeforeSubmit(
    actorId: string,
    kind: 'oneWay' | 'request',
    signal?: AbortSignal
  ): Promise<number | void> {
    const entry = [...this.entries.values()].find(candidate =>
      candidate.actorId === actorId
      && candidate.kind === kind
      && candidate.capturedCount === 0
    );
    if (entry === undefined) return;
    entry.capturedCount += 1;
    entry.markCaptured();
    if (signal?.aborted === true) throw signal.reason;
    return await entry.released;
  }

  async waitCaptured(operationId: string): Promise<void> {
    const entry = this.require(operationId);
    await entry.captured;
  }

  release(operationId: string, submissionCopies = 1): void {
    const entry = this.require(operationId);
    entry.releasedCount += 1;
    entry.release(Math.max(1, submissionCopies));
  }

  snapshot(operationId: string): { capturedCount: number; releasedCount: number } {
    const entry = this.require(operationId);
    return {
      capturedCount: entry.capturedCount,
      releasedCount: entry.releasedCount
    };
  }

  recordMessageFollowRelay(actorId: string, context: ZLinkActorMessageFollowContext): void {
    const scenario = actorScenarios.get(actorId);
    if (scenario !== undefined) {
      evidence.add(scenario, actorId, 'message_follow_relay_context', JSON.stringify(context));
    }
  }

  hasPending(actorId: string, kind: 'oneWay' | 'request'): boolean {
    return [...this.entries.values()].some(entry =>
      entry.actorId === actorId
      && entry.kind === kind
      && entry.capturedCount === 0
    );
  }

  private require(operationId: string) {
    const entry = this.entries.get(operationId);
    if (entry === undefined) throw new Error(`Transport delivery '${operationId}' is not armed.`);
    return entry;
  }
}

class ApplyActorLifecycleState {
  constructor(readonly actorType: string, readonly stateVersion: number) {}
}

class TransferActor implements ZLinkActor {
  actorType: string = SpotActorTransferNames.actorTypeStateful;
  stateVersion = 0;
  applicationState = new Uint8Array();
  readonly context!: ZLinkActorContext;

  constructor(readonly actorId: string, context?: ZLinkActorContext) {
    if (context !== undefined) Object.defineProperty(this, 'context', { value: context, configurable: true });
  }

  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    joinCompletions.set(this.actorId, completion);
    const operationId = `${completion.operationId.high}:${completion.operationId.low}`;
    let scenario = actorScenarios.get(this.actorId) ?? 'deferred-join';
    let acceptedReply: JoinTargetRes | undefined;
    if (completion.status === 'accepted' && completion.reply !== undefined) {
      acceptedReply = completion.reply.decode<JoinTargetRes>(Object as never);
      scenario = actorScenarios.get(this.actorId) ?? acceptedReply.scenario;
    }
    if (
      completion.status === 'accepted'
      && scenario === 'ST-H2'
      && actorScenarios.has(this.actorId)
    ) {
      evidence.add(scenario, this.actorId, 'join_completion_started', operationId);
      await completionGates.wait(this.actorId);
    }
    if (acceptedReply !== undefined) {
      evidence.add(
        scenario,
        this.actorId,
        'commit_ack',
        acceptedReply.targetSpotId
      );
    }
    evidence.add(
      scenario,
      this.actorId,
      'join_completion',
      completion.status === 'failed'
        ? `${completion.status}|${operationId}|kind=${completion.kind}`
        : `${completion.status}|${operationId}`
    );
  }
}

@zlinkSpotActorSendHandler({
  actor: () => TransferActor,
  spot: () => TransferUserSpot,
  packetName: 'ApplyActorLifecycleState'
})
@zlinkEntrySpotActorSendHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: 'ApplyActorLifecycleState'
})
class ApplyActorLifecycleStateHandler {
  @ZLinkSpotActorSend('ApplyActorLifecycleState')
  async handle(
    _spot: TransferUserSpot | TransferRecoveryUserSpot | TransferEntrySpot,
    actor: TransferActor,
    context: ZLinkMessageContext,
    message: ApplyActorLifecycleState
  ): Promise<void> {
    actor.actorType = message.actorType;
    actor.stateVersion = message.stateVersion;
  }
}

class NoAdapterActor extends TransferActor {}

@Injectable()
class TransferActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<TransferActor> {
    const actorId = context.actorId;
    actorLifecycleStates.set(actorId, {
      actorType: SpotActorTransferNames.actorTypeStateful,
      stateVersion: 0
    });
    const actor = new TransferActor(actorId, context);
    if (actorId.startsWith('actor-h3-')) {
      evidence.add('ST-H3', actorId, 'context_identity',
        actor.context === context ? 'same' : 'different');
    }
    return actor;
  }
}

@Injectable()
class NoAdapterActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<NoAdapterActor> {
    const actorId = context.actorId;
    if (options.rid === 'actor-b') evidence.add('transfer', actorId, 'transfer_in_empty_default', 'actor-factory');
    const actor = new NoAdapterActor(actorId, context);
    actor.actorType = SpotActorTransferNames.actorTypeNoAdapter;
    actorLifecycleStates.set(actorId, { actorType: actor.actorType, stateVersion: actor.stateVersion });
    return actor;
  }
}

@Injectable()
class TransferActorAdapter implements ZLinkActorRelocationAdapter<TransferActor> {
  async capture(actor: TransferActor, signal: AbortSignal): Promise<Uint8Array> {
    signal.throwIfAborted();
    if (actor.actorType === SpotActorTransferNames.actorTypeFailTransferOut) {
      evidence.add('ST-C3', actor.actorId, 'transfer_out_failed', String(actor.stateVersion));
      throw new Error('injected transfer out failure');
    }
    if (actor.actorType === SpotActorTransferNames.actorTypeEmptyState) {
      evidence.add('transfer', actor.actorId, 'transfer_out_empty', 'custom-adapter');
      return new Uint8Array();
    }
    evidence.add('transfer', actor.actorId, 'transfer_out', String(actor.stateVersion));
    if (actor.actorId.startsWith('actor-handoff-gate-')) {
      const scenario = actorScenarios.get(actor.actorId) ?? 'ST-F1';
      evidence.add(scenario, actor.actorId, 'before_commit_gate', String(actor.stateVersion));
      await transferGates.wait(actor.actorId, signal);
    }
    actorLifecycleStates.set(actor.actorId, { actorType: actor.actorType, stateVersion: actor.stateVersion });
    const header = new TextEncoder().encode(JSON.stringify({
      actorId: actor.actorId,
      actorType: actor.actorType,
      stateVersion: actor.stateVersion,
      applicationStateBytes: actor.applicationState.byteLength
    } satisfies TransferStateDto) + '\n');
    const encoded = new Uint8Array(header.byteLength + actor.applicationState.byteLength);
    encoded.set(header);
    encoded.set(actor.applicationState, header.byteLength);
    if (actorScenarios.get(actor.actorId) === 'ST-I1') {
      evidence.add('ST-I1', actor.actorId, 'payload_captured',
        `application=${actor.applicationState.byteLength}|encoded=${encoded.byteLength}`);
    }
    return encoded;
  }

  async restore(actor: TransferActor, payload: Uint8Array, signal: AbortSignal): Promise<void> {
    signal.throwIfAborted();
    if (payload.byteLength === 0) {
      evidence.add('transfer', actor.actorId, 'transfer_in_empty', 'custom-adapter');
      actor.actorType = SpotActorTransferNames.actorTypeEmptyState;
      return;
    }
    const separator = payload.indexOf(10);
    if (separator < 0) throw new Error('Relocation payload header is missing.');
    const dto = JSON.parse(
      new TextDecoder().decode(payload.subarray(0, separator))
    ) as TransferStateDto;
    if (actor.actorId.startsWith('actor-fail-transfer-in-')) {
      evidence.add('ST-C3', actor.actorId, 'transfer_in_failed', String(dto.stateVersion));
      throw new Error('injected transfer in failure');
    }
    actor.actorType = dto.actorType;
    actor.stateVersion = dto.stateVersion;
    actor.applicationState = payload.slice(separator + 1);
    if (actor.applicationState.byteLength !== (dto.applicationStateBytes ?? 0)) {
      throw new Error('Relocation application state size changed during restore.');
    }
    actorLifecycleStates.set(actor.actorId, { actorType: actor.actorType, stateVersion: actor.stateVersion });
    evidence.add('transfer', actor.actorId, 'transfer_in', String(actor.stateVersion));
    if (actorScenarios.get(actor.actorId) === 'ST-I1') {
      evidence.add('ST-I1', actor.actorId, 'payload_restored',
        `application=${actor.applicationState.byteLength}|encoded=${payload.byteLength}`);
    }
  }
}

@Injectable()
class TransferEntrySpot implements ZLinkEntrySpot<TransferActor> {
  readonly context!: ZLinkEntrySpotContext<TransferActor>;

  async onCreateActor(actor: TransferActor, request: ZLinkMessage): Promise<{ accepted: boolean }> {
    let actorType = actor.actorType;
    let stateVersion = 0;
    let applicationStateBytes = 0;
    if (!request.toEncodedPayload().isEmpty()) {
      const create = request.decode<ActorCreateReq>(Object as never);
      actorType = create.actorType;
      stateVersion = create.stateVersion;
      applicationStateBytes = create.applicationStateBytes ?? 0;
      if (actorType === SpotActorTransferNames.actorTypeEmptyState) {
        domainState.save(actor.actorId, stateVersion);
      }
    }
    actor.actorType = actorType;
    actor.stateVersion = stateVersion;
    actor.applicationState = deterministicState(applicationStateBytes);
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
    if (actor.actorType === SpotActorTransferNames.actorTypeNoAdapter) {
      evidence.add('transfer', actorId, 'transfer_out_empty_default', 'no-adapter');
    }
    if (actor.actorType === SpotActorTransferNames.actorTypeFailLeave) {
      evidence.add('ST-C3', actorId, 'leave_failed', String(state?.stateVersion ?? 0));
      throw new Error('injected source leave failure');
    }
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

  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}

  async onCreate(request: ZLinkMessage): Promise<{ accepted: boolean }> {
    if (!request.toEncodedPayload().isEmpty()) this.mode = request.decode<CreateSpotReq>(Object as never).mode ?? 'accept';
    evidence.add('create_spot', String(this.context.spotId), 'spot_created', this.mode);
    return { accepted: true };
  }

  async onActorJoin(actorId: string, request: ZLinkMessage): Promise<{ accepted: boolean; reply: JoinTargetRes }> {
    const join = request.decode<JoinTargetReq>(Object as never);
    evidence.correlate(actorId, join.transferId);
    this.scenarios.set(actorId, join.scenario);
    actorScenarios.set(actorId, join.scenario);
    evidence.add(join.scenario, actorId, 'admission', `spot=${this.context.spotId}|mode=${this.mode}|input=actor-id-only`);
    if (join.scenario === 'ST-C1') {
      await transferGates.wait(actorId);
    }
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
    if (this.mode === 'delay-joined') {
      evidence.add(scenario, actorId, 'joined_wait', String(this.context.spotId));
      await joinedGates.wait(String(this.context.spotId));
      evidence.add(scenario, actorId, 'joined_released', String(this.context.spotId));
    }
    if (this.mode === 'fail-joined') {
      evidence.add(scenario, actorId, 'joined_failed', String(this.context.spotId));
      throw new Error('injected joined failure');
    }
    const current = actorLifecycleStates.get(actorId) ?? { actorType: actor.actorType, stateVersion: 0 };
    evidence.add('transfer', actorId, 'joined', `${this.context.spotId}:${current.stateVersion}`);
    if (actor.actorType === SpotActorTransferNames.actorTypeEmptyState) {
      const stateVersion = domainState.load(actorId);
      actorLifecycleStates.set(actorId, { actorType: actor.actorType, stateVersion });
      await this.actors
        .sendToActor(
          actor.actorId,
          new ApplyActorLifecycleState(actor.actorType, stateVersion)
        )
        .submit();
      evidence.add('transfer', actorId, 'domain_state_loaded', actorId);
    }
  }

  async onLeaveActor(actor: TransferActor): Promise<void> {
    evidence.add('transfer', actor.actorId, 'target_leave', String(this.context.spotId));
  }

  async onDisconnectActor(actor: TransferActor): Promise<void> { void actor; }
}

@Injectable()
class TransferRecoveryUserSpot extends TransferUserSpot {
  constructor(@Inject(ZLINK_ACTOR_CLIENT) actors: ZLinkActorClient) {
    super(actors);
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: SpotActorTransferNames.packetJoin
})
class JoinTargetHandler implements
  ZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, JoinTargetReq, JoinTargetRes> {
  @ZLinkSpotActorRequest(SpotActorTransferNames.packetJoin)
  async handle(
    _spot: TransferEntrySpot,
    actor: TransferActor,
    _context: ZLinkMessageContext,
    request: JoinTargetReq
  ): Promise<JoinTargetRes> {
    evidence.correlate(actor.actorId, request.transferId);
    actorScenarios.set(actor.actorId, request.scenario);
    const join = actor.context.joinSpot(request.targetSpotId, request).timeout(10000);
    join.defer();
    evidence.add(request.scenario, actor.actorId, 'join_deferred', request.targetSpotId);
    if (request.scenario === 'ST-H4') {
      try {
        join.defer();
      } catch (error) {
        evidence.add(request.scenario, actor.actorId, 'duplicate_defer_rejected', errorKind(error));
      }
      try {
        actor.context.joinSpot(request.targetSpotId, request).defer();
      } catch (error) {
        evidence.add(request.scenario, actor.actorId, 'pending_transition_rejected', errorKind(error));
      }
    }
    if (request.scenario === 'ST-H1') {
      (request as { targetSpotId: string }).targetSpotId = 'mutated-after-defer';
      evidence.add(request.scenario, actor.actorId, 'request_mutated_after_defer', request.targetSpotId);
    }
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
  packetName: SpotActorTransferNames.packetJoin
})
class UserJoinTargetHandler implements
  ZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, JoinTargetReq, JoinTargetRes> {
  @ZLinkSpotActorRequest(SpotActorTransferNames.packetJoin)
  async handle(
    _spot: TransferUserSpot,
    actor: TransferActor,
    context: ZLinkMessageContext,
    request: JoinTargetReq
  ): Promise<JoinTargetRes> {
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
  packetName: SpotActorTransferNames.packetProbe
})
class ProbeHandler implements
  ZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, ProbeReq, ProbeRes> {
  @ZLinkSpotActorRequest(SpotActorTransferNames.packetProbe)
  async handle(
    _spot: TransferUserSpot,
    actor: TransferActor,
    context: ZLinkMessageContext,
    request: ProbeReq
  ): Promise<ProbeRes> {
    evidence.add(
      request.scenario,
      actor.actorId,
      actor.context.spotId === undefined ? 'entry_packet_handler' : 'packet_handler',
      request.marker
    );
    if (request.scenario === 'ST-H5') {
      evidence.add(request.scenario, actor.actorId, 'request_context', JSON.stringify({
        meshName: context.meshName,
        channelName: context.channelName,
        packetName: context.packetName,
        contentType: context.contentType,
        metadata: [...context.metadata.values],
        correlationId: context.correlationId
      }));
    }
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
  packetName: SpotActorTransferNames.packetHandoff
})
class HandoffHandler implements
  ZLinkSpotActorSendHandler<TransferUserSpot, TransferActor, HandoffProbe> {
  @ZLinkSpotActorSend(SpotActorTransferNames.packetHandoff)
  async handle(
    _spot: TransferUserSpot,
    actor: TransferActor,
    context: ZLinkMessageContext,
    message: HandoffProbe
  ): Promise<void> {
    evidence.add(
      message.scenario,
      actor.actorId,
      actor.context.spotId === undefined ? 'entry_packet_handler' : 'packet_handler',
      message.marker
    );
    if (message.scenario === 'ST-H5') {
      evidence.add(message.scenario, actor.actorId, 'send_context', JSON.stringify({
        meshName: context.meshName,
        channelName: context.channelName,
        packetName: context.packetName,
        contentType: context.contentType,
        metadata: [...context.metadata.values],
        correlationId: context.correlationId
      }));
    }
  }
}

@Injectable()
@zlinkEntrySpotActorSendHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: SpotActorTransferNames.packetHandoff
})
class EntryHandoffHandler implements
  ZLinkEntrySpotActorSendHandler<TransferEntrySpot, TransferActor, HandoffProbe> {
  @ZLinkSpotActorSend(SpotActorTransferNames.packetHandoff)
  async handle(
    _spot: TransferEntrySpot,
    actor: TransferActor,
    context: ZLinkMessageContext,
    message: HandoffProbe
  ): Promise<void> {
    evidence.add(message.scenario, actor.actorId, 'entry_packet_handler', message.marker);
    if (message.scenario === 'ST-H5') {
      evidence.add(message.scenario, actor.actorId, 'send_context', JSON.stringify({
        meshName: context.meshName,
        channelName: context.channelName,
        packetName: context.packetName,
        contentType: context.contentType,
        metadata: [...context.metadata.values],
        correlationId: context.correlationId
      }));
    }
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => TransferActor,
  entrySpot: () => TransferEntrySpot,
  packetName: SpotActorTransferNames.packetProbe
})
class EntryProbeHandler implements
  ZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, ProbeReq, ProbeRes> {
  @ZLinkSpotActorRequest(SpotActorTransferNames.packetProbe)
  async handle(
    _spot: TransferEntrySpot,
    actor: TransferActor,
    context: ZLinkMessageContext,
    request: ProbeReq
  ): Promise<ProbeRes> {
    evidence.add(request.scenario, actor.actorId, 'entry_packet_handler', request.marker);
    if (request.scenario === 'ST-H5') {
      evidence.add(request.scenario, actor.actorId, 'request_context', JSON.stringify({
        meshName: context.meshName,
        channelName: context.channelName,
        packetName: context.packetName,
        contentType: context.contentType,
        metadata: [...context.metadata.values],
        correlationId: context.correlationId
      }));
    }
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
  packetName: SpotActorTransferNames.packetBoundPush
})
class EntryBoundPushHandler implements
  ZLinkEntrySpotActorRequestHandler<TransferEntrySpot, TransferActor, BoundPushReq, BoundPushRes> {
  @ZLinkSpotActorRequest(SpotActorTransferNames.packetBoundPush)
  async handle(
    _spot: TransferEntrySpot,
    actor: TransferActor,
    _context: ZLinkMessageContext,
    request: BoundPushReq
  ): Promise<BoundPushRes> {
    return await pushBound(actorContextLocation(actor), actor, request);
  }
}

@Injectable()
@zlinkSpotActorRequestHandler({
  actor: () => TransferActor,
  spot: () => TransferUserSpot,
  packetName: SpotActorTransferNames.packetBoundPush
})
class BoundPushHandler implements
  ZLinkSpotActorRequestHandler<TransferUserSpot, TransferActor, BoundPushReq, BoundPushRes> {
  @ZLinkSpotActorRequest(SpotActorTransferNames.packetBoundPush)
  async handle(
    _spot: TransferUserSpot,
    actor: TransferActor,
    _context: ZLinkMessageContext,
    request: BoundPushReq
  ): Promise<BoundPushRes> {
    return await pushBound(actorContextLocation(actor), actor, request);
  }
}

function actorContextLocation(actor: TransferActor): { readonly spotId: unknown; readonly nodeRid: unknown } {
  return {
    spotId: actor.context.spotId ?? options.rid,
    nodeRid: options.rid
  };
}

function errorKind(error: unknown): string {
  return error instanceof ZLinkFrameworkException
    ? String(error.kind)
    : error instanceof Error
      ? error.message
      : String(error);
}

async function pushBound(context: { spotId: unknown; nodeRid: unknown }, actor: TransferActor, request: BoundPushReq): Promise<BoundPushRes> {
  const response = probeResponse(context, actor, request);
  try {
    await actor.context.boundSession.send(new BoundPushNotify(
      response.scenario,
      response.actorId,
      response.spotId,
      response.nodeRid,
      response.stateVersion,
      response.marker
    )).submit();
    evidence.add(request.scenario, actor.actorId, 'bound_push', request.marker);
  } catch (error) {
    if (
      request.scenario !== 'ST-E1A'
      || !(error instanceof ZLinkFrameworkException)
      || error.kind !== ZLinkFrameworkErrorKind.InvalidOperation
    ) {
      throw error;
    }
    evidence.add(request.scenario, actor.actorId, 'bound_push_rejected', errorKind(error));
  }
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
const configuration = createSpotActorTransferConfigurationModule(
  SPOT_ACTOR_TRANSFER_OPTIONS,
  validateServerOptions
);
Module({
  imports: [
    configuration,
    ZLinkModule.forRootFactory({
      imports: [configuration],
      inject: [SPOT_ACTOR_TRANSFER_OPTIONS],
      useFactory: (value: unknown) => {
        options = value as ServerOptions;
        fs.mkdirSync(options.logDir, { recursive: true });
        evidence = new EvidenceStore(options.rid, options.evidenceFile);
        domainState = new DomainStateStore(options.logDir);
        joinedGates = new GateStore();
        transferGates = new GateStore();
        completionGates = new GateStore();
        const builder = zlinkFramework();
        const store = new ZLinkRedisLocationStore({
          url: `redis://${options.redisEndpoint}`,
          keyPrefix: `${options.redisKeyPrefix}:location`
        });
        const relocationStore = new ZLinkRedisRelocationStore({
          url: `redis://${options.redisEndpoint}`,
          keyPrefix: `${options.redisKeyPrefix}:relocation`
        });
        builder.addLocationStore(store);
        builder.addRelocationStore(relocationStore);
        builder.configureLocations()
          .pollingIntervalMs(100)
          .ownerLeaseRenewIntervalMs(1000)
          .ownerLeaseTtlMs(5000)
          .ownerLeaseFencingMarginMs(500)
          .ownerLeaseRenewTimeoutMs(500)
          .routeCacheMaxAgeMs(500)
          .messageFollowDurationMs(6000);
        builder.configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(path.join(options.logDir, `${options.rid}-flow.log`))
          .traceLabel(options.rid);
        const mesh = builder.addRouteMesh(SpotActorTransferNames.mesh)
          .listen(options.routerEndpoint).routingId(options.rid);
        const objects = mesh.objects().server();
        objects.addEntrySpot(TransferEntrySpot);
        objects.addActorFactory(
          SpotActorTransferNames.actorTypeStateful,
          TransferActorFactory,
          (factory) => factory.preserveStateWith(TransferActorAdapter)
        );
        objects.addActorFactory(
          SpotActorTransferNames.actorTypeEmptyState,
          TransferActorFactory,
          (factory) => factory.preserveStateWith(TransferActorAdapter)
        );
        objects.addActorFactory(
          SpotActorTransferNames.actorTypeFailTransferOut,
          TransferActorFactory,
          (factory) => factory.preserveStateWith(TransferActorAdapter)
        );
        objects.addActorFactory(
          SpotActorTransferNames.actorTypeFailLeave,
          TransferActorFactory,
          (factory) => factory.preserveStateWith(TransferActorAdapter)
        );
        objects.addActorFactory(
          SpotActorTransferNames.actorTypeFailTransferIn,
          TransferActorFactory,
          (factory) => factory.preserveStateWith(TransferActorAdapter)
        );
        objects.addActorFactory(
          SpotActorTransferNames.actorTypeNoAdapter,
          NoAdapterActorFactory,
          (factory) => factory.recreateOnRelocation()
        );
        objects.addSpotFactory(
          TransferUserSpot.name,
          TransferUserSpot,
          (factory) => factory.recreateOnRelocation()
        );
        if (options.rid === 'actor-b') {
          objects.addSpotFactory(
            'TransferRecoveryUserSpot',
            TransferRecoveryUserSpot,
            (factory) => {
              factory.executionMode(ZLinkUserSpotExecutionMode.PerActor);
              factory.recreateOnRelocation();
            }
          );
        }
        mesh.channel(SpotActorTransferNames.mesh).server();
        return builder.build();
      }
    })
  ],
  providers: [
    TransferActorFactory,
    NoAdapterActorFactory,
    TransferActorAdapter,
    TransferEntrySpot,
    TransferUserSpot,
    TransferRecoveryUserSpot,
    JoinTargetHandler,
    UserJoinTargetHandler,
    ProbeHandler,
    HandoffHandler,
    EntryProbeHandler,
    EntryHandoffHandler,
    EntryBoundPushHandler,
    BoundPushHandler,
    ApplyActorLifecycleStateHandler,
    TransportDeliveryGate,
    {
      provide: TRANSPORT_DELIVERY_GATE,
      useExisting: TransportDeliveryGate
    }
  ]
})(ActorNodeModule);

let actorManager: ZLinkActorManager;

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(ActorNodeModule, { logger: false, abortOnError: false });
  actorManager = app.get(ZLINK_ACTOR_MANAGER, { strict: false }) as ZLinkActorManager;
  const actorClient = app.get(ZLINK_ACTOR_CLIENT, { strict: false }) as ZLinkActorClient;
  const transportDeliveryGate = app.get(TransportDeliveryGate, { strict: false });
  const locationQuery = app.get(
    ZLINK_LOCATION_RUNTIME_QUERY,
    { strict: false }
  ) as ZLinkLocationRuntimeQuery;
  const routeMeshRuntime = app.get(
    ZLINK_ROUTE_MESH_RUNTIME,
    { strict: false }
  ) as ZLinkRouteMeshRuntime;
  const frameworkRuntime = app.get(
    ZLINK_FRAMEWORK_RUNTIME,
    { strict: false }
  ) as ZLinkFrameworkRuntime;
  const spots = app.get(ZLINK_SPOT_MANAGER, { strict: false }) as ZLinkSpotManager;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok', rid: options.rid }) },
    {
      method: 'GET',
      path: '/mesh-snapshot',
      handle: () => meshSnapshot(routeMeshRuntime, locationQuery)
    },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST', path: '/relocate', handle: async (body) => {
        const deadlineMs = Math.max(
          1,
          Math.min(Number((body as { deadlineMs?: number }).deadlineMs ?? 120_000), 300_000)
        );
        const startedAt = Date.now();
        const result = await frameworkRuntime.relocate({
          mode: ZLinkFrameworkRelocationMode.PlannedMaintenance,
          deadlineMs
        });
        return {
          relocated: result.outcome === ZLinkFrameworkRelocationOutcome.Relocated,
          outcome: String(result.outcome),
          reason: String(result.reason),
          elapsedMs: Date.now() - startedAt
        };
      }
    },
    {
      method: 'GET', path: /^\/spots\/([^/]+)\/ref$/, handle: async (_body, match) => {
        const spot = await spots.find(match![1]);
        return spot === undefined ? { found: false } : {
          found: true,
          spotId: String(spot.spotId),
          nodeRid: String(spot.nodeRid),
          objectGeneration: spot.objectGeneration.toString()
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
      method: 'POST', path: /^\/transport-delivery\/([^/]+)\/arm$/, handle: (body, match) => {
        const request = body as {
          actorId: string;
          kind: 'oneWay' | 'request';
        };
        transportDeliveryGate.arm(match![1], request.actorId, request.kind);
        return transportDeliveryGate.snapshot(match![1]);
      }
    },
    {
      method: 'POST', path: /^\/transport-delivery\/([^/]+)\/wait$/, handle: async (_body, match) => {
        await transportDeliveryGate.waitCaptured(match![1]);
        return transportDeliveryGate.snapshot(match![1]);
      }
    },
    {
      method: 'POST', path: /^\/transport-delivery\/([^/]+)\/release$/, handle: (body, match) => {
        const request = body as { submissionCopies?: number };
        transportDeliveryGate.release(match![1], request.submissionCopies);
        return transportDeliveryGate.snapshot(match![1]);
      }
    },
    {
      method: 'GET', path: /^\/transport-delivery\/([^/]+)$/, handle: (_body, match) =>
        transportDeliveryGate.snapshot(match![1])
    },
    {
      method: 'POST', path: /^\/joined-gates\/([^/]+)\/release$/, handle: (_body, match) => ({
        key: match![1], released: joinedGates.release(match![1])
      } satisfies GateReleaseRes)
    },
    {
      method: 'POST', path: /^\/transfer-gates\/([^/]+)\/release$/, handle: (_body, match) => ({
        key: match![1], released: transferGates.release(match![1])
      } satisfies GateReleaseRes)
    },
    {
      method: 'POST', path: '/spots', handle: async (body) => {
        const request = body as CreateSpotReq;
        const stableType = request.mode === 'restart-recovery'
          ? 'TransferRecoveryUserSpot'
          : TransferUserSpot.name;
        const result = await spots
          .getOrCreate(request.spotId, stableType)
          .inMesh(SpotActorTransferNames.mesh)
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
      method: 'POST', path: '/actors', handle: async (body) => {
        const request = body as ActorCreateReq;
        const result = await actorManager
          .getOrCreate(request.actorId, request.actorType)
          .inMesh(SpotActorTransferNames.mesh)
          .request(request)
          .submit();
        if (result.status === 'rejected') {
          throw new Error(`Actor '${request.actorId}' creation was rejected.`);
        }
        return {
          actorId: result.actor.actorId,
          actorType: request.actorType,
          objectGeneration: result.actor.objectGeneration.toString(),
          meshName: result.actor.meshName,
          nodeRid: String(result.actor.nodeRid)
        } satisfies ActorCreateRes;
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/destroy$/, handle: async (_body, match) => {
        const actorId = match![1];
        const actor = await requireActor(actorId);
        const destroyed = await actorManager.destroy(actor);
        actorScenarios.delete(actorId);
        actorLifecycleStates.delete(actorId);
        return { actorId, destroyed };
      }
    },
    {
      method: 'GET', path: /^\/actors\/([^/]+)\/ref$/, handle: async (_body, match) => {
        const actor = await requireActor(match![1]);
        return actorRefResponse(actor);
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
        const input = body as ProbeReq & { metadata?: Record<string, string> };
        const request = new ProbeReq(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs);
        const fixtureMode = transportDeliveryGate.hasPending(match![1], 'request');
        const call = actorClient.requestToActor(match![1], request)
          .timeout(request.requestTimeoutMs ?? 10000);
        for (const [key, value] of Object.entries(input.metadata ?? {})) {
          call.metadata(key, value);
        }
        if (!fixtureMode) return await call.submit<ProbeRes>();
        try {
          return {
            succeeded: true,
            response: await call.submit<ProbeRes>()
          };
        } catch (error) {
          return {
            succeeded: false,
            errorKind: error instanceof ZLinkFrameworkException
              ? error.kind
              : error instanceof Error
                ? error.name
                : 'Unknown'
          };
        }
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/handoff$/, handle: async (body, match) => {
        const input = body as HandoffProbe & { metadata?: Record<string, string> };
        const call = actorClient.sendToActor(
          match![1],
          new HandoffProbe(input.scenario, input.marker, input.delayMs, input.requestTimeoutMs)
        );
        for (const [key, value] of Object.entries(input.metadata ?? {})) {
          call.metadata(key, value);
        }
        await call.submit();
        return { accepted: true };
      }
    },
    {
      method: 'POST', path: /^\/actors\/([^/]+)\/bound-push$/, handle: async (body, match) => {
        const input = body as BoundPushReq;
        return actorClient.requestToActor(
          match![1],
          new BoundPushReq(input.scenario, input.marker)
        )
          .timeout(10000).submit<BoundPushRes>();
      }
    },
    {
      method: 'POST', path: '/crash', handle: () => {
        setTimeout(() => process.exit(86), 25);
        return { status: 'crashing' };
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);
  while (!stopping) await new Promise((resolve) => setTimeout(resolve, 100));
  await closeHttpServer(server);
  await app.close();
}

async function requireActor(actorId: string): Promise<ActorRef> {
  const actor = await actorManager.find(actorId);
  if (actor === undefined) throw new Error(`Actor '${actorId}' was not found.`);
  return actor;
}

function actorRefResponse(actor: ActorRef): ActorRefRes {
  return {
    actorId: actor.actorId,
    objectGeneration: actor.objectGeneration.toString(),
    meshName: actor.meshName,
    nodeRid: String(actor.nodeRid)
  };
}

function deterministicState(byteLength: number): Uint8Array<ArrayBuffer> {
  if (!Number.isSafeInteger(byteLength) || byteLength < 0 || byteLength > 64 * 1024 * 1024) {
    throw new Error('applicationStateBytes must be an integer in 0..64 MiB.');
  }
  const state = new Uint8Array(byteLength);
  for (let index = 0; index < state.length; index++) {
    state[index] = (index * 31 + 17) & 0xff;
  }
  return state;
}

async function meshSnapshot(
  runtime: ZLinkRouteMeshRuntime,
  locationQuery: ZLinkLocationRuntimeQuery
): Promise<{
  readonly rid: string;
  readonly ready: boolean;
  readonly readyPeerRids: readonly string[];
  readonly peers: readonly {
    readonly rid: string;
    readonly state: string;
    readonly ready: boolean;
    readonly lastFailure?: string;
  }[];
  readonly topologyRids: readonly string[];
  readonly topology: readonly {
    readonly rid: string;
    readonly endpoint: string;
    readonly state: string;
    readonly draining: boolean;
  }[];
}> {
  const snapshot = runtime.snapshot(SpotActorTransferNames.mesh);
  const topology = await locationQuery.listTopology(
    { meshName: SpotActorTransferNames.mesh },
    { pageSize: 100 }
  );
  return {
    rid: options.rid,
    ready: runtime.isReady(SpotActorTransferNames.mesh),
    readyPeerRids: snapshot.peers
      .filter(peer => peer.state === ZLinkPeerState.Ready)
      .map(peer => String(peer.nodeRid)),
    peers: snapshot.peers.map(peer => ({
      rid: String(peer.nodeRid),
      state: String(peer.state),
      ready: peer.state === ZLinkPeerState.Ready,
      lastFailure: peer.unavailableReason === undefined
        ? undefined
        : String(peer.unavailableReason)
    })),
    topologyRids: topology.items.map(entry => String(entry.nodeRid)),
    topology: topology.items.map(entry => ({
      rid: String(entry.nodeRid),
      endpoint: entry.endpoint,
      state: String(entry.state),
      draining: entry.draining
    }))
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

class DomainStateStore {
  constructor(private readonly directory: string) {}
  save(actorId: string, stateVersion: number): void {
    fs.writeFileSync(path.join(this.directory, `domain-${actorId}.state`), String(stateVersion));
  }
  load(actorId: string): number {
    return Number(fs.readFileSync(path.join(this.directory, `domain-${actorId}.state`), 'utf8'));
  }
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
