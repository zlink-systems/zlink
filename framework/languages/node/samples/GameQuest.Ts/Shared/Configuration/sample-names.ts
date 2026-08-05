const SampleNames = {
  playerStreamNode: 'gamequest-stream',
  playerActorType: 'gamequest-player',
  playerQuestSpotType: 'gamequest.player-quest',
  playerQuestSpotMesh: 'gamequest.player-quest.spot',
  requestTimeout: 5000,
  clientTimeout: 20000
} as const;

function questMissionSpotId(playerId: string): string {
  return `player-quest-${playerId}`;
}

export {
  SampleNames,
  questMissionSpotId
};
