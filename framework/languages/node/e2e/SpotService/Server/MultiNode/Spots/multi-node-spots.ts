import { Inject, Injectable } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorFactory,
  ZLinkActorJoinCompletion,
  ZLinkActorRelocationAdapter,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessageContext,
  ZLinkRouteClient,
  ZLinkRouteRequestHandler,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotManager,
  ZLinkSpotOutbound,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import {
  ZLINK_ROUTE_CLIENT,
  ZLINK_SPOT_MANAGER,
  zlinkEntrySpotActorRequestHandler
} from '@zlink-systems/nestjs';
import { ZLinkMessage, ZLinkPacket, ZLinkSpotActorRequest } from '@zlink-systems/framework';
import type {
  ActorPingReq,
  ActorPingRes,
  MultiNodeCreateSpotRes,
  MultiNodeCreateSpotReq,
  ScaleOutActorProbeReq,
  ScaleOutActorProbeRes,
  SpotOnlyJoinReq,
  SpotOnlyJoinRes,
  SpotOnlyMeshReq,
  SpotOnlyMeshRes,
  StateRes
} from '../../../Shared/messages';
import { SpotServiceNames, StateMsg, StateReq, spotServicePacket } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

export class MultiNodeSpotA implements ZLinkSpot {
  private static evidence?: EvidenceStore;
  private value = 0;
  readonly context!: ZLinkSpotContext;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  configure(): void {
    this.context.handlers.addPacket(MultiNodeStateAHandler);
  }

  async onInitialize(): Promise<void> {
    MultiNodeSpotA.requireEvidence()
      .add(`multi-spot-initialize|node=${SpotServiceNames.multiSpotNodeA}|spot=${this.context.spotId}`);
  }

  async onActorJoin(): Promise<{ accepted: boolean }> { return { accepted: true }; }
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
  async onDisconnectActor(): Promise<void> {}

  add(delta: number): number {
    this.value += delta;
    return this.value;
  }

  static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('MultiNodeSpotA evidence store is not configured.');
    }
    return this.evidence;
  }
}

export class MultiNodeSpotB implements ZLinkSpot {
  private static evidence?: EvidenceStore;
  private value = 0;
  readonly context!: ZLinkSpotContext;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  configure(): void {
    this.context.handlers.addPacket(MultiNodeStateBHandler);
  }

  async onInitialize(): Promise<void> {
    MultiNodeSpotB.requireEvidence()
      .add(`multi-spot-initialize|node=${SpotServiceNames.multiSpotNodeB}|spot=${this.context.spotId}`);
  }

  async onActorJoin(): Promise<{ accepted: boolean }> { return { accepted: true }; }
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
  async onDisconnectActor(): Promise<void> {}

  add(delta: number): number {
    this.value += delta;
    return this.value;
  }

  static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('MultiNodeSpotB evidence store is not configured.');
    }
    return this.evidence;
  }
}

export class MultiNodeScenarioActor implements ZLinkActor {
  readonly context: ZLinkActorContext;

  constructor(readonly actorId: string, context?: ZLinkActorContext) {
    this.context = context as ZLinkActorContext;
  }

  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    const evidence = MultiNodeEntrySpot.requireEvidence();
    evidence.add(
      `spot-only-actor-join-completed|rid=${evidence.rid}|actor=${this.actorId}`
      + `|accepted=${completion.status === 'accepted'}`
    );
  }
}

export class MultiNodeScenarioActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<ZLinkActor> {
    return new MultiNodeScenarioActor(context.actorId, context);
  }
}

export class MultiNodeScenarioActorRelocationAdapter
implements ZLinkActorRelocationAdapter<MultiNodeScenarioActor> {
  async capture(actor: MultiNodeScenarioActor, signal: AbortSignal): Promise<Uint8Array> {
    signal.throwIfAborted();
    return new TextEncoder().encode(JSON.stringify({ actorId: actor.actorId }));
  }

  async restore(
    actor: MultiNodeScenarioActor,
    payload: Uint8Array,
    signal: AbortSignal
  ): Promise<void> {
    signal.throwIfAborted();
    const state = JSON.parse(new TextDecoder().decode(payload)) as { actorId: string };
    if (state.actorId !== actor.actorId) {
      throw new Error(`Actor relocation payload mismatch for '${actor.actorId}'.`);
    }
  }
}

export class MultiNodeEntrySpot implements ZLinkEntrySpot<MultiNodeScenarioActor> {
  private static evidence?: EvidenceStore;
  readonly context!: ZLinkEntrySpotContext<MultiNodeScenarioActor>;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  async onCreateActor(actor: MultiNodeScenarioActor): Promise<{ accepted: boolean }> {
    MultiNodeEntrySpot.requireEvidence()
      .add(`entry-created|rid=${MultiNodeEntrySpot.requireEvidence().rid}|actor=${actor.actorId}`);
    return { accepted: true };
  }

  async onJoinedActor(actor: MultiNodeScenarioActor): Promise<void> {
    MultiNodeEntrySpot.requireEvidence()
      .add(`entry-joined|rid=${MultiNodeEntrySpot.requireEvidence().rid}|actor=${actor.actorId}`);
  }

  async onLeaveActor(_actor: MultiNodeScenarioActor): Promise<void> {}
  async onDisconnectActor(_actor: MultiNodeScenarioActor): Promise<void> {}

  static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('MultiNodeEntrySpot evidence store is not configured.');
    }
    return this.evidence;
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => MultiNodeScenarioActor,
  entrySpot: () => MultiNodeEntrySpot,
  packetName: 'ActorPingReq'
})
export class MultiNodeEntryActorPingHandler {
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('ActorPingReq')
  async handle(
    _spot: MultiNodeEntrySpot,
    actor: MultiNodeScenarioActor,
    _context: ZLinkMessageContext,
    request: ActorPingReq
  ): Promise<ActorPingRes> {
    this.evidence.add(
      `actor-pingMsg|rid=${this.evidence.rid}|actor=${actor.actorId}`
      + `|spot=${actor.context.spotId ?? this.evidence.rid}|value=${request.value}`
    );
    return {
      actorId: actor.actorId,
      nodeRid: this.evidence.rid,
      spotId: String(actor.context.spotId ?? this.evidence.rid),
      value: request.value,
      seen: 1
    };
  }
}

export class SpotOnlyUserSpot implements ZLinkSpot<MultiNodeScenarioActor> {
  private static evidence?: EvidenceStore;
  private static refs?: ZLinkSpotManager;
  private value = 0;
  readonly context!: ZLinkSpotContext<MultiNodeScenarioActor>;

  static configureDependencies(evidence: EvidenceStore, refs: ZLinkSpotManager): void {
    this.evidence = evidence;
    this.refs = refs;
  }

  configure(): void {
    this.context.handlers.addPacket(SpotOnlyStateReqHandler);
    this.context.handlers.addPacket(SpotOnlyStateMsgHandler);
  }

  async onInitialize(): Promise<void> {
    SpotOnlyUserSpot.requireEvidence()
      .add(`spot-initialize|rid=${SpotOnlyUserSpot.requireEvidence().rid}|spot=${this.context.spotId}`);
  }

  async onCreate(request: ZLinkMessage): Promise<{ accepted: boolean; reply?: StateRes }> {
    SpotOnlyUserSpot.requireEvidence()
      .add(`spot-created|rid=${SpotOnlyUserSpot.requireEvidence().rid}|spot=${this.context.spotId}`);
    const command = request.decode<SpotOnlyMeshReq | undefined>(Object as never);
    if (command !== undefined) {
      return { accepted: true, reply: await this.requestSend(command) };
    }
    return { accepted: true };
  }

  async requestSend(request: SpotOnlyMeshReq): Promise<StateRes> {
    const target = await SpotOnlyUserSpot.requireRefs().find(request.targetSpotId);
    if (target === undefined) {
      throw new Error(`Spot '${request.targetSpotId}' was not found.`);
    }
    const reply = await requestSpotOnlyState(
      this.context.outbound,
      target.spotId,
      { operation: 'add', delta: 7 } satisfies StateReq
    );
    await sendSpotOnlyState(
      this.context.outbound,
      target.spotId,
      { marker: `sm-f6-send-${request.marker}` } satisfies StateMsg
    );
    SpotOnlyUserSpot.requireEvidence().add(
      `spot-only-request|rid=${SpotOnlyUserSpot.requireEvidence().rid}|source=${this.context.spotId}`
      + `|target=${request.targetSpotId}|value=${reply.value}|marker=${request.marker}`
    );
    return reply;
  }

  async onActorJoin(actorId: string): Promise<{ accepted: boolean }> {
    SpotOnlyUserSpot.requireEvidence()
      .add(`spot-actor-admitted|rid=${SpotOnlyUserSpot.requireEvidence().rid}|spot=${this.context.spotId}|actor=${actorId}`);
    return { accepted: true };
  }

  async onJoinedActor(actor: MultiNodeScenarioActor): Promise<void> {
    SpotOnlyUserSpot.requireEvidence()
      .add(`spot-actor-joined|rid=${SpotOnlyUserSpot.requireEvidence().rid}|spot=${this.context.spotId}|actor=${actor.actorId}`);
  }

  async onLeaveActor(_actor: MultiNodeScenarioActor): Promise<void> {}
  async onDisconnectActor(_actor: MultiNodeScenarioActor): Promise<void> {}

  add(delta: number): number {
    this.value += delta;
    return this.value;
  }

  static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('SpotOnlyUserSpot evidence store is not configured.');
    }
    return this.evidence;
  }

  static requireRefs(): ZLinkSpotManager {
    if (this.refs === undefined) {
      throw new Error('SpotOnlyUserSpot refs are not configured.');
    }
    return this.refs;
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => MultiNodeScenarioActor,
  entrySpot: () => MultiNodeEntrySpot,
  packetName: 'ScaleOutActorProbeReq'
})
export class ScaleOutActorProbeHandler
{
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('ScaleOutActorProbeReq')
  async handle(
    _spot: MultiNodeEntrySpot,
    actor: MultiNodeScenarioActor,
    context: ZLinkMessageContext,
    request: ScaleOutActorProbeReq
  ): Promise<ScaleOutActorProbeRes> {
    void context;
    this.evidence.add(
      `scale-out-actor-probe|rid=${this.evidence.rid}|spot=${actor.context.spotId ?? this.evidence.rid}`
      + `|actor=${actor.actorId}|marker=${request.marker}`
    );
    return { actorId: actor.actorId, nodeRid: this.evidence.rid, marker: request.marker };
  }
}

async function requestSpotOnlyState(
  outbound: ZLinkSpotOutbound,
  targetSpotId: string,
  request: StateReq
): Promise<StateRes> {
  return await outbound
    .requestToSpot(targetSpotId, spotServicePacket(StateReq, request))
    .timeout(2000)
    .submit<StateRes>();
}

async function sendSpotOnlyState(
  outbound: ZLinkSpotOutbound,
  targetSpotId: string,
  message: StateMsg
): Promise<void> {
  await outbound
    .sendToSpot(targetSpotId, spotServicePacket(StateMsg, message))
    .submit();
}

@Injectable()
export class MultiNodeCreateSpotAHandler implements ZLinkRouteRequestHandler<MultiNodeCreateSpotReq, MultiNodeCreateSpotRes> {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER)
    private readonly spots: ZLinkSpotManager,
    @Inject(ZLINK_ROUTE_CLIENT)
    private readonly routes: ZLinkRouteClient,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(request: MultiNodeCreateSpotReq): Promise<MultiNodeCreateSpotRes> {
    const result = await this.spots
      .getOrCreate(request.spotId, MultiNodeSpotA.name)
      .inMesh(SpotServiceNames.multiSpotNodeA)
      .submit();
    const state = await requestState(this.routes, SpotServiceNames.multiRouteChannelA, request.spotId, request.delta);
    this.evidence.add(`multi-create-spot|node=${SpotServiceNames.multiSpotNodeA}|spot=${result.spot.spotId}|state=${result.state}`);
    return {
      spotId: String(result.spot.spotId),
      nodeRid: SpotServiceNames.multiSpotNodeA,
      state: String(result.state),
      value: state.value
    };
  }
}

@Injectable()
export class MultiNodeCreateSpotBHandler implements ZLinkRouteRequestHandler<MultiNodeCreateSpotReq, MultiNodeCreateSpotRes> {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER)
    private readonly spots: ZLinkSpotManager,
    @Inject(ZLINK_ROUTE_CLIENT)
    private readonly routes: ZLinkRouteClient,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(request: MultiNodeCreateSpotReq): Promise<MultiNodeCreateSpotRes> {
    const result = await this.spots
      .getOrCreate(request.spotId, MultiNodeSpotB.name)
      .inMesh(SpotServiceNames.multiSpotNodeB)
      .submit();
    const state = await requestState(this.routes, SpotServiceNames.multiRouteChannelB, request.spotId, request.delta);
    this.evidence.add(`multi-create-spot|node=${SpotServiceNames.multiSpotNodeB}|spot=${result.spot.spotId}|state=${result.state}`);
    return {
      spotId: String(result.spot.spotId),
      nodeRid: SpotServiceNames.multiSpotNodeB,
      state: String(result.state),
      value: state.value
    };
  }
}

@Injectable()
@ZLinkPacket('StateReq')
export class MultiNodeStateAHandler implements ZLinkSpotRequestHandler<MultiNodeSpotA, StateReq, StateRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: MultiNodeSpotA, request: StateReq, context: ZLinkMessageContext): Promise<StateRes> {
    void context;
    const value = spot.add(request.operation === 'add' ? request.delta : 0);
    this.evidence.add(`multi-state-request|node=${SpotServiceNames.multiSpotNodeA}|spot=${spot.context.spotId}|value=${value}`);
    return {
      spotId: String(spot.context.spotId),
      nodeRid: String(spot.context.nodeRid),
      value
    };
  }
}

@Injectable()
@ZLinkPacket('StateReq')
export class MultiNodeStateBHandler implements ZLinkSpotRequestHandler<MultiNodeSpotB, StateReq, StateRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: MultiNodeSpotB, request: StateReq, context: ZLinkMessageContext): Promise<StateRes> {
    void context;
    const value = spot.add(request.operation === 'add' ? request.delta : 0);
    this.evidence.add(`multi-state-request|node=${SpotServiceNames.multiSpotNodeB}|spot=${spot.context.spotId}|value=${value}`);
    return {
      spotId: String(spot.context.spotId),
      nodeRid: String(spot.context.nodeRid),
      value
    };
  }
}

@Injectable()
@ZLinkPacket('StateReq')
export class SpotOnlyStateReqHandler implements ZLinkSpotRequestHandler<SpotOnlyUserSpot, StateReq, StateRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: SpotOnlyUserSpot, request: StateReq, context: ZLinkMessageContext): Promise<StateRes> {
    void context;
    const value = spot.add(request.operation === 'add' ? request.delta : 0);
    this.evidence.add(`spot-state-request|rid=${this.evidence.rid}|spot=${spot.context.spotId}|value=${value}`);
    return {
      spotId: String(spot.context.spotId),
      nodeRid: String(spot.context.nodeRid),
      value
    };
  }
}

@Injectable()
@ZLinkPacket('StateMsg')
export class SpotOnlyStateMsgHandler implements ZLinkSpotPacketHandler<SpotOnlyUserSpot, StateMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(spot: SpotOnlyUserSpot, message: StateMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(`spot-state-command|rid=${this.evidence.rid}|spot=${spot.context.spotId}|marker=${message.marker}`);
  }
}

@Injectable()
@zlinkEntrySpotActorRequestHandler({
  actor: () => MultiNodeScenarioActor,
  entrySpot: () => MultiNodeEntrySpot,
  packetName: 'SpotOnlyJoinReq'
})
export class MultiNodeSpotOnlyJoinHandler
{
  constructor(private readonly evidence: EvidenceStore) {}

  @ZLinkSpotActorRequest('SpotOnlyJoinReq')
  async handle(
    _spot: MultiNodeEntrySpot,
    actor: MultiNodeScenarioActor,
    context: ZLinkMessageContext,
    request: SpotOnlyJoinReq
  ): Promise<SpotOnlyJoinRes> {
    void context;
    actor.context
      .joinSpot(request.targetSpotId, request)
      .timeout(10000)
      .defer();
    this.evidence.add(
      `spot-only-actor-join|rid=${this.evidence.rid}|actor=${actor.actorId}`
      + `|target=${request.targetSpotId}|accepted=true|marker=${request.marker}`
    );
    return {
      targetSpotId: request.targetSpotId,
      actorId: actor.actorId,
      accepted: true,
      marker: request.marker
    };
  }
}

export async function createLocalMultiNodeSpot(
  spots: ZLinkSpotManager,
  evidence: EvidenceStore,
  nodeRid: string,
  spotId: string
): Promise<MultiNodeCreateSpotRes> {
  const created = nodeRid === SpotServiceNames.multiSpotNodeA
    ? await spots.getOrCreate(spotId, MultiNodeSpotA.name).inMesh(nodeRid).submit()
    : await spots.getOrCreate(spotId, MultiNodeSpotB.name).inMesh(nodeRid).submit();
  evidence.add(`multi-create-spot|node=${nodeRid}|spot=${created.spot.spotId}|state=${created.state}`);
  return {
    spotId: String(created.spot.spotId),
    nodeRid,
    state: String(created.state),
    value: 0
  };
}

export async function requestState(
  routes: ZLinkRouteClient,
  channelName: string,
  spotId: string,
  delta: number
): Promise<StateRes> {
  return await routes
    .requestToNode(channelName, spotId, spotServicePacket(StateReq, { operation: 'add', delta }))
    .timeout(2000)
    .submit<StateRes>();
}

export async function requestStateViaSpotOutbound(
  outbound: ZLinkSpotOutbound,
  spotRefs: ZLinkSpotManager,
  _meshName: string,
  spotId: string,
  delta: number
): Promise<StateRes> {
  const spot = await spotRefs.find(spotId);
  if (spot === undefined) {
    throw new Error(`SpotRef '${spotId}' was not found.`);
  }
  return await outbound
    .requestToSpot(spot.spotId, spotServicePacket(StateReq, { operation: 'add', delta }))
    .timeout(2000)
    .submit<StateRes>();
}
