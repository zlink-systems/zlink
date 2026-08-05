import { QuestIds, QuestStatuses } from '../../../Shared/Contracts/messages';
import type {
  GameplayEventEnvelope,
  QuestProgress,
  StoredQuestEvent
} from '../../../Shared/Contracts/messages';

type QuestState = {
  currentCount: number;
  requiredCount: number;
  status: string;
  version: number;
  updatedAtUnixMs: number;
  lastSourceEventId?: string;
  matchedConditions: Set<string>;
};

type ProposedQuestEvent = Omit<StoredQuestEvent, 'payload' | 'version'> & {
  payload: unknown;
  version?: number;
};

type QuestDecision = {
  events: ProposedQuestEvent[];
  changedQuestIds: string[];
  completedQuestIds: string[];
};

class PlayerQuestAggregate {
  private readonly quests = new Map<string, QuestState>();
  private readonly sourceEventIds = new Set<string>();

  static from(events: StoredQuestEvent[]): PlayerQuestAggregate {
    const aggregate = new PlayerQuestAggregate();
    for (const event of events) aggregate.fold(event);
    return aggregate;
  }

  hasSourceEvent(eventId: string): boolean {
    return this.sourceEventIds.has(eventId);
  }

  quest(questId: string): Readonly<QuestState> | undefined {
    return this.quests.get(questId);
  }

  projection(playerId: string): QuestProgress[] {
    return [...this.quests.entries()].map(([questId, state]) => ({
      playerId,
      questId,
      status: state.status,
      currentCount: state.currentCount,
      requiredCount: state.requiredCount,
      lastSourceEventId: state.lastSourceEventId,
      version: state.version,
      updatedAtUnixMs: state.updatedAtUnixMs
    }));
  }

  private fold(event: StoredQuestEvent): void {
    if (event.sourceEventId !== undefined) this.sourceEventIds.add(event.sourceEventId);
    const payload = event.payload;
    const current = this.quests.get(event.questId);
    if (event.type === 'QuestProgressed' &&
        typeof payload.currentCount === 'number' && typeof payload.requiredCount === 'number') {
      const conditions = new Set(current?.matchedConditions ?? []);
      if (typeof payload.conditionKey === 'string') conditions.add(payload.conditionKey);
      this.quests.set(event.questId, {
        currentCount: payload.currentCount,
        requiredCount: payload.requiredCount,
        status: current?.status ?? QuestStatuses.Active,
        version: event.version,
        updatedAtUnixMs: event.createdAtUnixMs,
        lastSourceEventId: event.sourceEventId,
        matchedConditions: conditions
      });
      return;
    }
    if (event.type === 'QuestReconciled' && typeof payload.currentCount === 'number') {
      this.quests.set(event.questId, {
        currentCount: payload.currentCount,
        requiredCount: current?.requiredCount ?? requiredCountFor(event.questId),
        // Reconcile records corrected progress. Completion and reward require their own events.
        status: current?.status ?? QuestStatuses.Active,
        version: event.version,
        updatedAtUnixMs: event.createdAtUnixMs,
        lastSourceEventId: current?.lastSourceEventId,
        matchedConditions: new Set(current?.matchedConditions ?? [])
      });
      return;
    }
    if (current === undefined) return;
    if (event.type === 'QuestCompleted') {
      this.quests.set(event.questId, { ...current, status: QuestStatuses.Completed, version: event.version, updatedAtUnixMs: event.createdAtUnixMs });
    } else if (event.type === 'QuestRewardGranted') {
      this.quests.set(event.questId, { ...current, status: QuestStatuses.RewardGranted, version: event.version, updatedAtUnixMs: event.createdAtUnixMs });
    }
  }
}

const QuestDomain = {
  decide(event: GameplayEventEnvelope, aggregate: PlayerQuestAggregate): QuestDecision {
    if (aggregate.hasSourceEvent(event.eventId)) return emptyDecision();
    const decisions: QuestDecision[] = [];
    if (event.type === 'MonsterKilled' && event.payload.value === 'wolf:forest') {
      decisions.push(counterDecision(event, aggregate, QuestIds.FirstHunt, event.payload.count, 3));
    }
    if (event.type === 'ItemCollected' && event.payload.value === 'healing-herb') {
      decisions.push(counterDecision(event, aggregate, QuestIds.HerbGathering, event.payload.count, 5));
    }
    if (event.type === 'FeatureUnlocked' && event.payload.value === 'auction') {
      decisions.push(conditionDecision(event, aggregate, QuestIds.OpenAuction, 'auction', 1));
      decisions.push(conditionDecision(event, aggregate, QuestIds.TutorialPath, 'auction', 2));
    }
    if (event.type === 'MissionCompleted' && event.payload.value === 'tutorial') {
      decisions.push(conditionDecision(event, aggregate, QuestIds.TutorialPath, 'tutorial', 2));
      decisions.push(orderedDecision(event, aggregate, QuestIds.RuinsExplorer, 0, 2));
    }
    if (event.type === 'AreaEntered' && event.payload.value === 'ruins') {
      decisions.push(orderedDecision(event, aggregate, QuestIds.RuinsExplorer, 1, 2));
    }
    return mergeDecisions(decisions);
  },

  reconcileFirstHunt(
    playerId: string,
    factCount: number,
    aggregate: PlayerQuestAggregate
  ): QuestDecision {
    const current = aggregate.quest(QuestIds.FirstHunt);
    if (factCount <= 0 || (current?.currentCount ?? 0) === factCount) return emptyDecision();
    const now = Date.now();
    const sourceEventId = `reconcile-${playerId}-first-hunt-${now}`;
    const events: ProposedQuestEvent[] = [{
      eventId: sourceEventId,
      playerId,
      questId: QuestIds.FirstHunt,
      type: 'QuestReconciled',
      sourceEventId: undefined,
      payload: { playerId, questId: QuestIds.FirstHunt, currentCount: factCount, reason: 'sync', reconciledAtUnixMs: now },
      createdAtUnixMs: now
    }];
    const completed = (current?.currentCount ?? 0) < 3 && factCount >= 3 && current?.status !== QuestStatuses.RewardGranted;
    if (completed) events.push(...completionEvents(playerId, QuestIds.FirstHunt, sourceEventId, now));
    return {
      events,
      changedQuestIds: [QuestIds.FirstHunt],
      completedQuestIds: completed ? [QuestIds.FirstHunt] : []
    };
  },

  project(playerId: string, events: StoredQuestEvent[]): QuestProgress[] {
    return PlayerQuestAggregate.from(events).projection(playerId);
  }
};

function counterDecision(
  event: GameplayEventEnvelope,
  aggregate: PlayerQuestAggregate,
  questId: string,
  delta: number,
  requiredCount: number
): QuestDecision {
  const current = aggregate.quest(questId);
  if (current?.status === QuestStatuses.RewardGranted) return emptyDecision();
  return progressDecision(event, questId, (current?.currentCount ?? 0) + delta, requiredCount);
}

function conditionDecision(
  event: GameplayEventEnvelope,
  aggregate: PlayerQuestAggregate,
  questId: string,
  conditionKey: string,
  requiredCount: number
): QuestDecision {
  const current = aggregate.quest(questId);
  if (current?.status === QuestStatuses.RewardGranted || current?.matchedConditions.has(conditionKey) === true) return emptyDecision();
  return progressDecision(event, questId, (current?.currentCount ?? 0) + 1, requiredCount, conditionKey);
}

function orderedDecision(
  event: GameplayEventEnvelope,
  aggregate: PlayerQuestAggregate,
  questId: string,
  expectedStep: number,
  requiredCount: number
): QuestDecision {
  const current = aggregate.quest(questId);
  if (current?.status === QuestStatuses.RewardGranted || (current?.currentCount ?? 0) !== expectedStep) return emptyDecision();
  return progressDecision(event, questId, expectedStep + 1, requiredCount, `step-${expectedStep + 1}`);
}

function progressDecision(
  event: GameplayEventEnvelope,
  questId: string,
  currentCount: number,
  requiredCount: number,
  conditionKey?: string
): QuestDecision {
  const now = Date.now();
  const events: ProposedQuestEvent[] = [{
    eventId: `${questId}-${event.eventId}-progress`,
    sourceEventId: event.eventId,
    playerId: event.playerId,
    questId,
    type: 'QuestProgressed',
    payload: { playerId: event.playerId, questId, delta: event.payload.count, currentCount, requiredCount, sourceEventId: event.eventId, conditionKey },
    createdAtUnixMs: now
  }];
  const completed = currentCount >= requiredCount;
  if (completed) events.push(...completionEvents(event.playerId, questId, event.eventId, now));
  return { events, changedQuestIds: [questId], completedQuestIds: completed ? [questId] : [] };
}

function completionEvents(playerId: string, questId: string, sourceEventId: string, now: number): ProposedQuestEvent[] {
  return [{
    eventId: `${questId}-${sourceEventId}-completed`, sourceEventId, playerId, questId,
    type: 'QuestCompleted',
    payload: { playerId, questId, sourceEventId, completedAtUnixMs: now }, createdAtUnixMs: now
  }, {
    eventId: `${questId}-${sourceEventId}-reward`, sourceEventId, playerId, questId,
    type: 'QuestRewardGranted',
    payload: { playerId, questId, rewardId: `reward-${questId}`, grantedAtUnixMs: now }, createdAtUnixMs: now
  }];
}

function mergeDecisions(decisions: QuestDecision[]): QuestDecision {
  return {
    events: decisions.flatMap((decision) => decision.events),
    changedQuestIds: [...new Set(decisions.flatMap((decision) => decision.changedQuestIds))],
    completedQuestIds: [...new Set(decisions.flatMap((decision) => decision.completedQuestIds))]
  };
}

function emptyDecision(): QuestDecision {
  return { events: [], changedQuestIds: [], completedQuestIds: [] };
}

function requiredCountFor(questId: string): number {
  return questId === QuestIds.HerbGathering ? 5 : questId === QuestIds.FirstHunt ? 3 : 2;
}

export { PlayerQuestAggregate, QuestDomain };
export type { ProposedQuestEvent, QuestDecision };
