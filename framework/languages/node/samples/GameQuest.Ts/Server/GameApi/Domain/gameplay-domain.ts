import type { GameplayEventEnvelope } from '../../../Shared/Contracts/messages';

const GameplayDomain = {
  monsterKilled(playerId: string, monsterId: string, areaId: string, idempotencyKey: string, sourceApi: string): GameplayEventEnvelope {
    return createEvent(playerId, idempotencyKey, 'MonsterKilled', `${monsterId}:${areaId}`, 1, sourceApi);
  },
  itemCollected(playerId: string, itemId: string, count: number, idempotencyKey: string, sourceApi: string): GameplayEventEnvelope {
    if (!Number.isSafeInteger(count) || count <= 0 || count > 100) {
      throw new Error('Collected item count must be a positive integer no greater than 100.');
    }
    return createEvent(playerId, idempotencyKey, 'ItemCollected', itemId, count, sourceApi);
  },
  missionCompleted(playerId: string, missionId: string, idempotencyKey: string, sourceApi: string): GameplayEventEnvelope {
    return createEvent(playerId, idempotencyKey, 'MissionCompleted', missionId, 1, sourceApi);
  },
  areaEntered(playerId: string, areaId: string, idempotencyKey: string, sourceApi: string): GameplayEventEnvelope {
    return createEvent(playerId, idempotencyKey, 'AreaEntered', areaId, 1, sourceApi);
  },
  featureUnlocked(playerId: string, featureId: string, idempotencyKey: string, sourceApi: string): GameplayEventEnvelope {
    return createEvent(playerId, idempotencyKey, 'FeatureUnlocked', featureId, 1, sourceApi);
  }
};

function createEvent(
  playerId: string,
  idempotencyKey: string,
  type: string,
  value: string,
  count: number,
  sourceApi: string
): GameplayEventEnvelope {
  return {
    eventId: `${playerId}-${idempotencyKey}`,
    playerId,
    type,
    payload: { idempotencyKey, value, count, sourceApi },
    occurredAtUnixMs: Date.now()
  };
}

export { GameplayDomain };
