import { Inject, Injectable } from '@nestjs/common';
import type {
  ControlPingRes,
  ControlPingReq,
  CrossRoleActorPushReq,
  CrossRoleActorPushRes,
  CreateSpotRes,
  CreateSpotReq,
  EnsureActorRes,
  EnsureActorReq,
  ActorPingRes
} from '../../../Shared/messages';
import { ActorPushReq, SpotServiceNames } from '../../../Shared/messages';
import type {
  ZLinkActorClient,
  ZLinkActorManager,
  ZLinkSpotManager,
  ZLinkRouteMessageContext,
  ZLinkRouteRequestHandler
} from '@zlink-systems/framework';
import { ZLINK_ACTOR_CLIENT, ZLINK_ACTOR_MANAGER, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import { ScenarioUserSpot } from '../Spots/scenario-spots';

@Injectable()
export class ControlPingHandler implements ZLinkRouteRequestHandler<ControlPingReq, ControlPingRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: ControlPingReq, context: ZLinkRouteMessageContext): Promise<ControlPingRes> {
    void context;
    this.evidence.add(`control-pingMsg|rid=${this.evidence.rid}|value=${request.value}`);
    return {
      value: request.value,
      nodeRid: this.evidence.rid
    };
  }
}

@Injectable()
export class EnsureActorHandler implements ZLinkRouteRequestHandler<EnsureActorReq, EnsureActorRes> {
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(request: EnsureActorReq, context: ZLinkRouteMessageContext): Promise<EnsureActorRes> {
    void context;
    const created = await this.actors
      .getOrCreate(request.actorId, SpotServiceNames.actorType)
      .inMesh(request.meshName ?? SpotServiceNames.spotChannel)
      .request(request)
      .submit();
    if (created.status === 'rejected') {
      throw new Error(`Actor '${request.actorId}' creation was rejected.`);
    }
    const actorRef = created.actor;
    this.evidence.add(`ensure-actor|rid=${this.evidence.rid}|actor=${request.actorId}`);
    this.evidence.add(`entry-joined|rid=${this.evidence.rid}|actor=${request.actorId}`);
    return {
      actorId: actorRef.actorId,
      nodeRid: String(actorRef.nodeRid),
      generation: actorRef.objectGeneration.toString()
    };
  }
}

@Injectable()
export class CrossRoleActorPushHandler
  implements ZLinkRouteRequestHandler<CrossRoleActorPushReq, CrossRoleActorPushRes> {
  constructor(
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(
    request: CrossRoleActorPushReq,
    context: ZLinkRouteMessageContext
  ): Promise<CrossRoleActorPushRes> {
    void context;
    const reply = await this.actors
      .requestToActor(request.actorId, new ActorPushReq(request.value))
      .timeout(5000)
      .submit<ActorPingRes>();
    this.evidence.add(
      `cross-role-push|rid=${this.evidence.rid}|actor=${request.actorId}|value=${request.value}|seen=${reply.seen}`
    );
    return {
      actorId: reply.actorId,
      nodeRid: reply.nodeRid,
      value: reply.value,
      delivered: true
    };
  }
}

@Injectable()
export class CreateSpotHandler implements ZLinkRouteRequestHandler<CreateSpotReq, CreateSpotRes> {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(request: CreateSpotReq, context: ZLinkRouteMessageContext): Promise<CreateSpotRes> {
    void context;
    const created = await this.spots
      .getOrCreate(request.spotId, ScenarioUserSpot.name)
      .inMesh(SpotServiceNames.spotChannel)
      .submit();
    const state = typeof created.state === 'string' ? created.state : String(created.state);
    this.evidence.add(`create-spot|rid=${this.evidence.rid}|spot=${created.spot.spotId}|state=${state}`);
    return {
      spotId: String(created.spot.spotId),
      nodeRid: String(created.spot.nodeRid),
      state
    };
  }
}
