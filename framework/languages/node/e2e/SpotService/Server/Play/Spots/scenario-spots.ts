import type {
  ZLinkSpot,
  ZLinkMessage,
  ZLinkMessageContext,
  ZLinkSpotActorJoinResult,
  ZLinkSpotActorRequestHandler,
  ZLinkSpotContext
} from '@zlink-systems/framework';
import { ZLinkSpotActorRequest } from '@zlink-systems/framework';
import { zlinkSpotActorRequestHandler } from '@zlink-systems/nestjs';
import type {
  ActorPingRes,
  ActorPingReq,
  ActorPushReq,
  LeaveReq,
  LeaveRes
} from '../../../Shared/messages';
import { ActorPushNotify } from '../../../Shared/messages';
import { SpotServiceNames } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import { SpotMsgHandler, SpotOutboundHandler, SpotOutboundNegativeHandler } from '../Handlers/spot-outbound-handlers';
import { SpotToSpotHandler, SpotToSpotNegativeHandler, SpotToSpotTimeoutHandler } from '../Handlers/spot-to-spot-handlers';
import { StageProbeHandler, StageTimerStartHandler } from '../Handlers/stage-handlers';
import { SlowSpotHandler, StateCommandHandler, StateReqHandler } from '../Handlers/state-req-handler';
import { SpotAdminHandler } from '../Handlers/spot-admin-handler';
import { ScenarioActor } from './scenario-actors';

export class ScenarioUserSpot implements ZLinkSpot<ScenarioActor> {
  private static evidence?: EvidenceStore;
  readonly context!: ZLinkSpotContext<ScenarioActor, ScenarioUserSpot>;
  value = 0;

  static useEvidence(evidence: EvidenceStore): void {
    this.evidence = evidence;
  }

  configure(): void {
    this.context.handlers.addPacket(StateReqHandler);
    this.context.handlers.addPacket(StateCommandHandler);
    this.context.handlers.addPacket(StageProbeHandler);
    this.context.handlers.addPacket(StageTimerStartHandler);
    this.context.handlers.addPacket(SlowSpotHandler);
    this.context.handlers.addPacket(SpotOutboundHandler);
    this.context.handlers.addPacket(SpotOutboundNegativeHandler);
    this.context.handlers.addPacket(SpotToSpotHandler);
    this.context.handlers.addPacket(SpotToSpotTimeoutHandler);
    this.context.handlers.addPacket(SpotToSpotNegativeHandler);
    this.context.handlers.addPacket(SpotAdminHandler);
    this.context.handlers.addSubscribe(
      SpotMsgHandler,
      SpotServiceNames.spotChannel,
      SpotServiceNames.spotEventTopic
    );
  }

  async onInitialize(): Promise<void> {
    const evidence = ScenarioUserSpot.requireEvidence();
    evidence.add(`spot-initialize|rid=${evidence.rid}|spot=${this.context.spotId}`);
  }

  async onClosing(): Promise<void> {
    const evidence = ScenarioUserSpot.requireEvidence();
    evidence.add(`spot-closing|rid=${evidence.rid}|spot=${this.context.spotId}`);
  }

  add(delta: number): number {
    this.value += delta;
    return this.value;
  }

  async onActorJoin(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult> {
    const payload = request.decode<Partial<{ readonly actorId: string }>>(Object as never);
    if (payload.actorId?.includes('reject') === true) {
      const evidence = ScenarioUserSpot.requireEvidence();
      evidence.add(
        `spot-actor-join-rejected|rid=${this.context.nodeRid}|spot=${this.context.spotId}|actor=${actorId}`
      );
      return { accepted: false, reply: { accepted: false, actorId } };
    }
    return { accepted: true };
  }

  async onJoinedActor(actor: ScenarioActor): Promise<void> {
    const evidence = ScenarioUserSpot.requireEvidence();
    evidence.add(
      `spot-actor-joined|rid=${this.context.nodeRid}|spot=${this.context.spotId}|actor=${actor.actorId}`
    );
  }

  async onLeaveActor(actor: ScenarioActor): Promise<void> {
    const evidence = ScenarioUserSpot.requireEvidence();
    evidence.add(
      `spot-actor-left|rid=${this.context.nodeRid}|spot=${this.context.spotId}|actor=${actor.actorId}`
    );
  }

  async onDisconnectActor(actor: ScenarioActor): Promise<void> {
    const evidence = ScenarioUserSpot.requireEvidence();
    evidence.add(
      `spot-actor-disconnected|rid=${this.context.nodeRid}|spot=${this.context.spotId}|actor=${actor.actorId}`
    );
  }

  static requireEvidence(): EvidenceStore {
    if (this.evidence === undefined) {
      throw new Error('ScenarioUserSpot evidence store is not configured.');
    }
    return this.evidence;
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => ScenarioActor,
  spot: () => ScenarioUserSpot,
  packetName: 'UserActorPingReq'
})
export class UserActorPingHandler
  implements ZLinkSpotActorRequestHandler<ScenarioUserSpot, ScenarioActor, ActorPingReq, ActorPingRes> {
  @ZLinkSpotActorRequest('UserActorPingReq')
  async handle(
    _spot: ScenarioUserSpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: ActorPingReq
  ): Promise<ActorPingRes> {
    void context;
    actor.seen += 1;
    const evidence = ScenarioUserSpot.requireEvidence();
    evidence.add(
      `actor-pingMsg|rid=${evidence.rid}|actor=${actor.actorId}`
      + `|spot=${actor.context.spotId}|value=${request.value}|seen=${actor.seen}`
    );
    return {
      actorId: actor.actorId,
      nodeRid: evidence.rid,
      spotId: String(actor.context.spotId),
      value: request.value,
      seen: actor.seen
    };
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => ScenarioActor,
  spot: () => ScenarioUserSpot,
  packetName: 'UserActorPushReq'
})
export class UserActorPushHandler
  implements ZLinkSpotActorRequestHandler<ScenarioUserSpot, ScenarioActor, ActorPushReq, ActorPingRes> {
  @ZLinkSpotActorRequest('UserActorPushReq')
  async handle(
    _spot: ScenarioUserSpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: ActorPushReq
  ): Promise<ActorPingRes> {
    void context;
    actor.seen += 1;
    actor.context.boundSession
      .send(new ActorPushNotify(actor.actorId, request.value, actor.seen))
      .submit();
    const evidence = ScenarioUserSpot.requireEvidence();
    return {
      actorId: actor.actorId,
      nodeRid: evidence.rid,
      spotId: String(actor.context.spotId),
      value: request.value,
      seen: actor.seen
    };
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => ScenarioActor,
  spot: () => ScenarioUserSpot,
  packetName: 'LeaveReq'
})
export class UserActorLeaveHandler
  implements ZLinkSpotActorRequestHandler<ScenarioUserSpot, ScenarioActor, LeaveReq, LeaveRes> {
  @ZLinkSpotActorRequest('LeaveReq')
  async handle(
    _spot: ScenarioUserSpot,
    actor: ScenarioActor,
    context: ZLinkMessageContext,
    request: LeaveReq
  ): Promise<LeaveRes> {
    if (request.actorId !== actor.actorId) {
      throw new Error('Leave request actor does not match dispatched actor.');
    }
    actor.context.joinEntrySpot(request).timeout(5000).defer();
    return {
      actorId: actor.actorId,
      accepted: true
    };
  }
}

export class ScenarioAlternateSpot implements ZLinkSpot<ScenarioActor> {
  readonly context!: ZLinkSpotContext<ScenarioActor, ScenarioAlternateSpot>;
  async onActorJoin(): Promise<{ accepted: boolean }> { return { accepted: true }; }
  async onJoinedActor(): Promise<void> {}
  async onLeaveActor(): Promise<void> {}
  async onDisconnectActor(): Promise<void> {}
}
