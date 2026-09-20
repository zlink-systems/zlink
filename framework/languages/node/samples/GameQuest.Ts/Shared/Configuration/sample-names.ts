const SampleNames = {
  playerStreamNode: 'gamequest-stream',
  playerActorType: 'gamequest-player',
  playerQuestSpotType: 'gamequest.player-quest',
  playerQuestSpotMesh: 'gamequest.player-quest.spot',
  requestTimeout: 5000,
  clientTimeout: 20000
} as const;

const questMissionSpotIdPrefix = 'player-quest-';

function questMissionSpotId(playerId: string): string {
  return `${questMissionSpotIdPrefix}${playerId}`;
}

function playerIdFromQuestMissionSpotId(spotId: string): string {
  return spotId.slice(questMissionSpotIdPrefix.length);
}

export { SampleNames, questMissionSpotId, playerIdFromQuestMissionSpotId };
