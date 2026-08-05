import { Inject, Injectable } from '@nestjs/common';
import { GameplayDomain } from '../Domain/gameplay-domain';
import { GameplayEventPublisher } from '../Infrastructure/ZLink/gameplay-event-publisher';
import { GameplayStateStore } from '../../Shared/Store/quest-progress-store';
import { GAMEQUEST_INSTANCE_ID } from '../../Configuration/tokens';
import type {
  CollectItemReq,
  CollectItemRes,
  CompleteMissionReq,
  CompleteMissionRes,
  EnterAreaReq,
  EnterAreaRes,
  GameplayEventEnvelope,
  KillMonsterReq,
  KillMonsterRes,
  QuestProgress,
  UnlockFeatureReq,
  UnlockFeatureRes
} from '../../../Shared/Contracts/messages';

@Injectable()
class GameplayActionService {
  constructor(
    @Inject(GameplayStateStore) private readonly store: GameplayStateStore,
    @Inject(GameplayEventPublisher) private readonly publisher: GameplayEventPublisher,
    @Inject(GAMEQUEST_INSTANCE_ID) private readonly apiName: string
  ) {}

  async killMonster(request: KillMonsterReq): Promise<{ response: KillMonsterRes; projection: QuestProgress[]; completedQuestId?: string }> {
    return await this.publishAndNotify(GameplayDomain.monsterKilled(
      request.playerId,
      request.monsterId,
      request.areaId,
      request.idempotencyKey,
      this.apiName
    ));
  }

  async collectItem(request: CollectItemReq): Promise<{ response: CollectItemRes; projection: QuestProgress[]; completedQuestId?: string }> {
    return await this.publishAndNotify(GameplayDomain.itemCollected(
      request.playerId,
      request.itemId,
      request.count,
      request.idempotencyKey,
      this.apiName
    ));
  }

  async completeMission(request: CompleteMissionReq): Promise<{ response: CompleteMissionRes; projection: QuestProgress[]; completedQuestId?: string }> {
    return await this.publishAndNotify(GameplayDomain.missionCompleted(
      request.playerId,
      request.missionId,
      request.idempotencyKey,
      this.apiName
    ));
  }

  async enterArea(request: EnterAreaReq): Promise<{ response: EnterAreaRes; projection: QuestProgress[]; completedQuestId?: string }> {
    return await this.publishAndNotify(GameplayDomain.areaEntered(
      request.playerId,
      request.areaId,
      request.idempotencyKey,
      this.apiName
    ));
  }

  async unlockFeature(request: UnlockFeatureReq): Promise<{ response: UnlockFeatureRes; projection: QuestProgress[]; completedQuestId?: string }> {
    return await this.publishAndNotify(GameplayDomain.featureUnlocked(
      request.playerId,
      request.featureId,
      request.idempotencyKey,
      this.apiName
    ));
  }

  private async publishAndNotify<TResponse extends { eventId: string }>(
    candidate: GameplayEventEnvelope
  ): Promise<{ response: TResponse; projection: QuestProgress[]; completedQuestId?: string }> {
    const { event: stored, recorded } = this.store.recordGameplayEvent(candidate);
    await this.publisher.send(stored);
    if (recorded) {
      console.error(`gamequest api event routed api=${this.apiName} player=${stored.playerId} event=${stored.eventId}`);
    } else {
      console.error(`gamequest api event replayed api=${this.apiName} player=${stored.playerId} event=${stored.eventId}`);
    }
    return {
      response: { eventId: stored.eventId } as TResponse,
      projection: []
    };
  }
}

export { GameplayActionService };
