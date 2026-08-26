import { Inject, Injectable } from '@nestjs/common';
import { GameplayDomain } from '../Domain/gameplay-domain';
import { GameplayEventPublisher } from '../Infrastructure/ZLink/gameplay-event-publisher';
import { GameplayStateStore } from '../../Shared/Store/quest-progress-store';
import { GAMEQUEST_INSTANCE_ID } from '../../Configuration/tokens';
import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '@zlink-systems/framework';
import type {
  CollectItemMsg,
  CompleteMissionReq,
  CompleteMissionRes,
  EnterAreaMsg,
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

  async collectItem(message: CollectItemMsg): Promise<void> {
    await this.publishAndNotify(GameplayDomain.itemCollected(
      message.playerId,
      message.itemId,
      message.count,
      message.idempotencyKey,
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

  async enterArea(message: EnterAreaMsg): Promise<void> {
    await this.publishAndNotify(GameplayDomain.areaEntered(
      message.playerId,
      message.areaId,
      message.idempotencyKey,
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
    try {
      await this.publisher.send(stored);
    } catch (error) {
      if (
        error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.Unavailable
      ) {
        console.error(`gamequest-owner unavailable player=${stored.playerId}`);
      }
      throw error;
    }
    if (recorded) {
      console.error(`gamequest-api event-routed player=${stored.playerId}`);
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
