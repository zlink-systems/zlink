import { Inject, Injectable } from '@nestjs/common';
import {
  GameplayStateStore,
  QuestEventStore,
  QuestReadModelStore
} from '../../Shared/Store/quest-progress-store';
import { PlayerQuestAggregate, QuestDomain } from '../Domain/quest-domain';
import { GAMEQUEST_INSTANCE_ID } from '../../Configuration/tokens';
import type {
  GameplayEventEnvelope,
  QuestProgress,
  SyncQuestProgressReq,
  SyncQuestProgressRes
} from '../../../Shared/Contracts/messages';
import { QuestIds } from '../../../Shared/Contracts/messages';

type QuestProcessingResult = {
  aggregate: PlayerQuestAggregate;
  projection: QuestProgress[];
  changedProgress: QuestProgress[];
  completedQuestIds: string[];
};

@Injectable()
class QuestEventProcessor {
  constructor(
    @Inject(GameplayStateStore) private readonly gameplay: GameplayStateStore,
    @Inject(QuestEventStore) private readonly events: QuestEventStore,
    @Inject(QuestReadModelStore) private readonly readModel: QuestReadModelStore,
    @Inject(GAMEQUEST_INSTANCE_ID) private readonly missionName: string
  ) {}

  process(event: GameplayEventEnvelope, aggregate: PlayerQuestAggregate): QuestProcessingResult {
    const decision = QuestDomain.decide(event, aggregate);
    const result = this.commit(event.playerId, decision.events, decision.changedQuestIds, decision.completedQuestIds);
    for (const questId of decision.changedQuestIds) {
      console.error(`gamequest-mission processed player=${event.playerId} quest=${questId}`);
    }
    return result;
  }

  rehydrate(playerId: string): PlayerQuestAggregate {
    const events = this.events.rehydrate(playerId);
    const aggregate = PlayerQuestAggregate.from(events);
    this.readModel.project(playerId, events);
    return aggregate;
  }

  readProgress(playerId: string): QuestProgress[] {
    return this.readModel.readProjection(playerId);
  }

  syncProgress(request: SyncQuestProgressReq, aggregate: PlayerQuestAggregate): QuestProcessingResult & SyncQuestProgressRes {
    const snapshot = this.gameplay.readGameplaySnapshot(request.playerId);
    const firstHunt = snapshot.killCounts.find((entry) => entry.monsterId === 'wolf' && entry.areaId === 'forest')?.count ?? 0;
    const decision = QuestDomain.reconcileFirstHunt(request.playerId, firstHunt, aggregate);
    const result = this.commit(request.playerId, decision.events, decision.changedQuestIds, decision.completedQuestIds);
    if (decision.events.some((event) => event.type === 'QuestReconciled')) {
      console.error(`gamequest-mission reconciled player=${request.playerId} quest=${QuestIds.FirstHunt}`);
    }
    return { ...result, updatedQuests: result.projection };
  }

  consumeReplayAfterClose(playerId: string): boolean {
    return this.events.consumeReplayAfterClose(playerId);
  }

  private commit(
    playerId: string,
    proposed: Parameters<QuestEventStore['append']>[1],
    changedQuestIds: string[],
    completedQuestIds: string[]
  ): QuestProcessingResult {
    this.events.append(playerId, proposed, this.missionName);
    const stored = this.events.read(playerId);
    const aggregate = PlayerQuestAggregate.from(stored);
    const projection = this.readModel.project(playerId, stored);
    return {
      aggregate,
      projection,
      changedProgress: projection.filter((progress) => changedQuestIds.includes(progress.questId)),
      completedQuestIds
    };
  }
}

export { QuestEventProcessor };
export type { QuestProcessingResult };
