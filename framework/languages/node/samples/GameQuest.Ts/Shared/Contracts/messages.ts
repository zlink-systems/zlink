class KillMonsterReq {
  constructor(readonly playerId: string, readonly monsterId: string, readonly areaId: string, readonly idempotencyKey: string) {}
}
type KillMonsterRes = { eventId: string };
class CollectItemReq {
  constructor(readonly playerId: string, readonly itemId: string, readonly count: number, readonly idempotencyKey: string) {}
}
type CollectItemRes = { eventId: string };
class CompleteMissionReq {
  constructor(readonly playerId: string, readonly missionId: string, readonly idempotencyKey: string) {}
}
type CompleteMissionRes = { eventId: string };
class EnterAreaReq {
  constructor(readonly playerId: string, readonly areaId: string, readonly idempotencyKey: string) {}
}
type EnterAreaRes = { eventId: string };
class UnlockFeatureReq {
  constructor(readonly playerId: string, readonly featureId: string, readonly idempotencyKey: string) {}
}
type UnlockFeatureRes = { eventId: string };
class JoinSessionReq { constructor(readonly playerId: string) {} }
class JoinSessionRes {
  constructor(readonly playerId: string, readonly activeQuests: QuestProgress[]) {}
}
class GetQuestProgressReq { constructor(readonly playerId: string) {} }
type GetQuestProgressRes = { activeQuests: QuestProgress[] };
class SyncQuestProgressReq { constructor(readonly playerId: string) {} }
type SyncQuestProgressRes = { updatedQuests: QuestProgress[] };
class DeleteQuestProjectionReq { constructor(readonly playerId: string, readonly questId: string) {} }
type DeleteQuestProjectionRes = { deleted: boolean };
class RebuildQuestProjectionReq {
  constructor(readonly playerId: string, readonly questId: string) {}
}
class ClosePlayerQuestMsg {
  constructor(readonly reason?: string) {}
}
type GetGameplaySnapshotReq = { playerId: string };
type GetGameplaySnapshotRes = {
  playerId: string;
  killCounts: KillCountSnapshot[];
  itemCounts: ItemCountSnapshot[];
  completedMissionIds: string[];
  unlockedFeatureIds: string[];
  enteredAreaIds: string[];
  snapshotVersion: number;
};
type KillCountSnapshot = { monsterId: string; areaId?: string; count: number };
type ItemCountSnapshot = { itemId: string; count: number };
class QuestProgressNotify {
  constructor(readonly playerId: string, readonly progress: QuestProgress) {}
}
class QuestCompletedNotify {
  constructor(readonly playerId: string, readonly progress: QuestProgress, readonly rewardGranted: boolean) {}
}
type QuestProgress = {
  playerId: string;
  questId: string;
  status: string;
  currentCount: number;
  requiredCount: number;
  lastSourceEventId?: string;
  version: number;
  updatedAtUnixMs: number;
};
type GameplayEventPayload = {
  idempotencyKey: string;
  value: string;
  count: number;
  sourceApi: string;
};
type GameplayEventEnvelope = {
  eventId: string;
  playerId: string;
  type: string;
  payload: GameplayEventPayload;
  occurredAtUnixMs: number;
};
class GameplayMsg {
  constructor(
    readonly eventId: string,
    readonly playerId: string,
    readonly type: string,
    readonly payload: GameplayEventPayload,
    readonly occurredAtUnixMs: number
  ) {}
}
type QuestApplyResult = {
  applied: boolean;
  projection: QuestProgress[];
  changedProgress?: QuestProgress;
  completedQuestId?: string;
};
type QuestProgressedEvent = {
  playerId: string;
  questId: string;
  delta: number;
  currentCount: number;
  requiredCount: number;
  sourceEventId: string;
};
type QuestCompletedEvent = {
  playerId: string;
  questId: string;
  sourceEventId: string;
  completedAtUnixMs: number;
};
type QuestRewardGrantedEvent = {
  playerId: string;
  questId: string;
  rewardId: string;
  grantedAtUnixMs: number;
};
type QuestReconciledEvent = {
  playerId: string;
  questId: string;
  currentCount: number;
  reason: string;
  reconciledAtUnixMs: number;
};
type StoredQuestEvent = {
  eventId: string;
  sourceEventId?: string;
  playerId: string;
  questId: string;
  type: string;
  payload: Record<string, unknown>;
  version: number;
  createdAtUnixMs: number;
};
type GameQuestServerAssertRes = { passed: boolean; evidence: string[] };

const QuestIds = {
  FirstHunt: 'first-hunt',
  OpenAuction: 'open-auction',
  HerbGathering: 'herb-gathering',
  TutorialPath: 'tutorial-path',
  RuinsExplorer: 'ruins-explorer'
} as const;

const QuestStatuses = {
  Active: 'Active',
  Completed: 'Completed',
  RewardGranted: 'RewardGranted'
} as const;

const PacketNames = {
  killMonsterReq: 'KillMonsterReq',
  killMonsterRes: 'KillMonsterRes',
  collectItemReq: 'CollectItemReq',
  collectItemRes: 'CollectItemRes',
  completeMissionReq: 'CompleteMissionReq',
  completeMissionRes: 'CompleteMissionRes',
  enterAreaReq: 'EnterAreaReq',
  enterAreaRes: 'EnterAreaRes',
  unlockFeatureReq: 'UnlockFeatureReq',
  unlockFeatureRes: 'UnlockFeatureRes',
  joinSessionReq: 'JoinSessionReq',
  joinSessionRes: 'JoinSessionRes',
  getQuestProgressReq: 'GetQuestProgressReq',
  getQuestProgressRes: 'GetQuestProgressRes',
  syncQuestProgressReq: 'SyncQuestProgressReq',
  syncQuestProgressRes: 'SyncQuestProgressRes',
  deleteQuestProjectionReq: 'DeleteQuestProjectionReq',
  deleteQuestProjectionRes: 'DeleteQuestProjectionRes',
  rebuildQuestProjectionReq: 'RebuildQuestProjectionReq',
  gameplayMsg: 'GameplayMsg',
  closePlayerQuestMsg: 'ClosePlayerQuestMsg',
  questProgressNotify: 'QuestProgressNotify',
  questCompletedNotify: 'QuestCompletedNotify'
} as const;

function killMonsterReq(playerId: string, monsterId: string, areaId: string, idempotencyKey: string): KillMonsterReq {
  return new KillMonsterReq(playerId, monsterId, areaId, idempotencyKey);
}

function collectItemReq(playerId: string, itemId: string, count: number, idempotencyKey: string): CollectItemReq {
  return new CollectItemReq(playerId, itemId, count, idempotencyKey);
}

function completeMissionReq(playerId: string, missionId: string, idempotencyKey: string): CompleteMissionReq {
  return new CompleteMissionReq(playerId, missionId, idempotencyKey);
}

function enterAreaReq(playerId: string, areaId: string, idempotencyKey: string): EnterAreaReq {
  return new EnterAreaReq(playerId, areaId, idempotencyKey);
}

function unlockFeatureReq(playerId: string, featureId: string, idempotencyKey: string): UnlockFeatureReq {
  return new UnlockFeatureReq(playerId, featureId, idempotencyKey);
}

function joinSessionReq(playerId: string): JoinSessionReq {
  return new JoinSessionReq(playerId);
}

function getQuestProgressReq(playerId: string): GetQuestProgressReq {
  return new GetQuestProgressReq(playerId);
}

function syncQuestProgressReq(playerId: string): SyncQuestProgressReq {
  return new SyncQuestProgressReq(playerId);
}

function deleteQuestProjectionReq(playerId: string, questId: string): DeleteQuestProjectionReq {
  return new DeleteQuestProjectionReq(playerId, questId);
}

function rebuildQuestProjectionReq(playerId: string, questId: string): RebuildQuestProjectionReq {
  return new RebuildQuestProjectionReq(playerId, questId);
}

function gameplayMsg(event: GameplayEventEnvelope): GameplayMsg {
  return new GameplayMsg(event.eventId, event.playerId, event.type, event.payload, event.occurredAtUnixMs);
}

export {
  QuestProgressNotify,
  QuestCompletedNotify,
  KillMonsterReq,
  CollectItemReq,
  CompleteMissionReq,
  EnterAreaReq,
  UnlockFeatureReq,
  JoinSessionReq,
  JoinSessionRes,
  GetQuestProgressReq,
  SyncQuestProgressReq,
  DeleteQuestProjectionReq,
  RebuildQuestProjectionReq,
  ClosePlayerQuestMsg,
  GameplayMsg,
  PacketNames,
  QuestIds,
  QuestStatuses,
  killMonsterReq,
  collectItemReq,
  completeMissionReq,
  enterAreaReq,
  unlockFeatureReq,
  joinSessionReq,
  getQuestProgressReq,
  syncQuestProgressReq,
  deleteQuestProjectionReq,
  rebuildQuestProjectionReq,
  gameplayMsg
};

export type {
  KillMonsterRes,
  CollectItemRes,
  CompleteMissionRes,
  EnterAreaRes,
  UnlockFeatureRes,
  GetQuestProgressRes,
  SyncQuestProgressRes,
  DeleteQuestProjectionRes,
  GetGameplaySnapshotReq,
  GetGameplaySnapshotRes,
  KillCountSnapshot,
  ItemCountSnapshot,
  QuestProgress,
  GameplayEventEnvelope,
  GameplayEventPayload,
  QuestApplyResult,
  QuestProgressedEvent,
  QuestCompletedEvent,
  QuestRewardGrantedEvent,
  QuestReconciledEvent,
  StoredQuestEvent,
  GameQuestServerAssertRes
};
