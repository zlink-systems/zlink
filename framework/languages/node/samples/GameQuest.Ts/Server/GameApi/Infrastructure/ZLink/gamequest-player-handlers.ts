import { Inject } from '@nestjs/common';
import { ZLINK_SPOT_OUTBOUND, zlinkEntrySpotActorRequestHandler } from '@zlink-systems/nestjs';
import { GameplayActionService } from '../../Application/gameplay-action-service';
import { questMissionSpotId, SampleNames } from '../../../../Shared/Configuration/sample-names';
import { PacketNames, getQuestProgressReq, syncQuestProgressReq } from '../../../../Shared/Contracts/messages';
import { GameQuestEntrySpot } from './gamequest-entry-spot';
import { GameQuestPlayerActor } from './gamequest-player-actor';
import type {
  CollectItemReq,
  CompleteMissionReq,
  EnterAreaReq,
  GetQuestProgressReq,
  GetQuestProgressRes,
  KillMonsterReq,
  SyncQuestProgressReq,
  SyncQuestProgressRes,
  UnlockFeatureReq
} from '../../../../Shared/Contracts/messages';
import type {
  ZLinkEntrySpotActorRequestHandler,
  ZLinkSpotOutbound,
  ZLinkMessageContext
} from '@zlink-systems/framework';

abstract class GameQuestActionHandler<TRequest, TResponse>
  implements ZLinkEntrySpotActorRequestHandler<GameQuestEntrySpot, GameQuestPlayerActor, TRequest, TResponse> {
  constructor(protected readonly actions: GameplayActionService) {}
  abstract handle(
    spot: GameQuestEntrySpot,
    actor: GameQuestPlayerActor,
    context: ZLinkMessageContext,
    request: TRequest
  ): Promise<TResponse>;
  protected requirePlayer(actor: GameQuestPlayerActor, playerId: string): void {
    if (actor.actorId !== playerId) throw new Error(`Actor '${actor.actorId}' cannot act for player '${playerId}'.`);
  }
}

@zlinkEntrySpotActorRequestHandler({ actor: () => GameQuestPlayerActor, entrySpot: () => GameQuestEntrySpot, packetName: PacketNames.killMonsterReq })
class KillMonsterHandler extends GameQuestActionHandler<KillMonsterReq, { eventId: string }> {
  constructor(actions: GameplayActionService) { super(actions); }
  async handle(_spot: GameQuestEntrySpot, actor: GameQuestPlayerActor, _context: ZLinkMessageContext, request: KillMonsterReq) {
    this.requirePlayer(actor, request.playerId);
    return (await this.actions.killMonster(request)).response;
  }
}

@zlinkEntrySpotActorRequestHandler({ actor: () => GameQuestPlayerActor, entrySpot: () => GameQuestEntrySpot, packetName: PacketNames.collectItemReq })
class CollectItemHandler extends GameQuestActionHandler<CollectItemReq, { eventId: string }> {
  constructor(actions: GameplayActionService) { super(actions); }
  async handle(_spot: GameQuestEntrySpot, actor: GameQuestPlayerActor, _context: ZLinkMessageContext, request: CollectItemReq) {
    this.requirePlayer(actor, request.playerId);
    return (await this.actions.collectItem(request)).response;
  }
}

@zlinkEntrySpotActorRequestHandler({ actor: () => GameQuestPlayerActor, entrySpot: () => GameQuestEntrySpot, packetName: PacketNames.completeMissionReq })
class CompleteMissionHandler extends GameQuestActionHandler<CompleteMissionReq, { eventId: string }> {
  constructor(actions: GameplayActionService) { super(actions); }
  async handle(_spot: GameQuestEntrySpot, actor: GameQuestPlayerActor, _context: ZLinkMessageContext, request: CompleteMissionReq) {
    this.requirePlayer(actor, request.playerId);
    return (await this.actions.completeMission(request)).response;
  }
}

@zlinkEntrySpotActorRequestHandler({ actor: () => GameQuestPlayerActor, entrySpot: () => GameQuestEntrySpot, packetName: PacketNames.enterAreaReq })
class EnterAreaHandler extends GameQuestActionHandler<EnterAreaReq, { eventId: string }> {
  constructor(actions: GameplayActionService) { super(actions); }
  async handle(_spot: GameQuestEntrySpot, actor: GameQuestPlayerActor, _context: ZLinkMessageContext, request: EnterAreaReq) {
    this.requirePlayer(actor, request.playerId);
    return (await this.actions.enterArea(request)).response;
  }
}

@zlinkEntrySpotActorRequestHandler({ actor: () => GameQuestPlayerActor, entrySpot: () => GameQuestEntrySpot, packetName: PacketNames.unlockFeatureReq })
class UnlockFeatureHandler extends GameQuestActionHandler<UnlockFeatureReq, { eventId: string }> {
  constructor(actions: GameplayActionService) { super(actions); }
  async handle(_spot: GameQuestEntrySpot, actor: GameQuestPlayerActor, _context: ZLinkMessageContext, request: UnlockFeatureReq) {
    this.requirePlayer(actor, request.playerId);
    return (await this.actions.unlockFeature(request)).response;
  }
}

abstract class QuestOwnerRequestHandler<TRequest extends { playerId: string }, TResponse>
  implements ZLinkEntrySpotActorRequestHandler<GameQuestEntrySpot, GameQuestPlayerActor, TRequest, TResponse> {
  constructor(@Inject(ZLINK_SPOT_OUTBOUND) protected readonly spots: ZLinkSpotOutbound) {}
  protected requirePlayer(actor: GameQuestPlayerActor, playerId: string): void {
    if (actor.actorId !== playerId) throw new Error(`Actor '${actor.actorId}' cannot query player '${playerId}'.`);
  }
  protected request(playerId: string, request: object): Promise<TResponse> {
    return this.spots
      .requestToSpot(questMissionSpotId(playerId), request)
      .instanceSpot(SampleNames.playerQuestSpotType)
      .inMesh(SampleNames.playerQuestSpotMesh)
      .timeout(SampleNames.requestTimeout).submit<TResponse>();
  }
  abstract handle(
    spot: GameQuestEntrySpot,
    actor: GameQuestPlayerActor,
    context: ZLinkMessageContext,
    request: TRequest
  ): Promise<TResponse>;
}

@zlinkEntrySpotActorRequestHandler({ actor: () => GameQuestPlayerActor, entrySpot: () => GameQuestEntrySpot, packetName: PacketNames.getQuestProgressReq })
class GetQuestProgressHandler extends QuestOwnerRequestHandler<GetQuestProgressReq, GetQuestProgressRes> {
  constructor(@Inject(ZLINK_SPOT_OUTBOUND) spots: ZLinkSpotOutbound) { super(spots); }
  async handle(_spot: GameQuestEntrySpot, actor: GameQuestPlayerActor, _context: ZLinkMessageContext, request: GetQuestProgressReq) {
    this.requirePlayer(actor, request.playerId);
    return await this.request(request.playerId, getQuestProgressReq(request.playerId));
  }
}

@zlinkEntrySpotActorRequestHandler({ actor: () => GameQuestPlayerActor, entrySpot: () => GameQuestEntrySpot, packetName: PacketNames.syncQuestProgressReq })
class SyncQuestProgressHandler extends QuestOwnerRequestHandler<SyncQuestProgressReq, SyncQuestProgressRes> {
  constructor(@Inject(ZLINK_SPOT_OUTBOUND) spots: ZLinkSpotOutbound) { super(spots); }
  async handle(_spot: GameQuestEntrySpot, actor: GameQuestPlayerActor, _context: ZLinkMessageContext, request: SyncQuestProgressReq) {
    this.requirePlayer(actor, request.playerId);
    return await this.request(request.playerId, syncQuestProgressReq(request.playerId));
  }
}

export {
  KillMonsterHandler,
  CollectItemHandler,
  CompleteMissionHandler,
  EnterAreaHandler,
  UnlockFeatureHandler,
  GetQuestProgressHandler,
  SyncQuestProgressHandler
};
