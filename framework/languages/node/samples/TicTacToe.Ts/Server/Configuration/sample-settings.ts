const SampleNames = Object.freeze({
  apiChannel: 'tictactoe.api',
  playStream: 'tictactoe.play.stream',
  clientStreamNode: 'client.stream',
  playSpotNode: 'tictactoe',
  gameSpotType: 'tictactoe-game',
  playerActorType: 'player',
  playerMilestoneChannel: 'tictactoe.player.milestone.channel',
  playerMilestoneTopic: 'tictactoe.player.milestone'
});

const SampleTimings = Object.freeze({
  requestTimeout: 7000
});

const SampleDefaults = Object.freeze({
  requiredLevel: 3
});

export { SampleDefaults, SampleNames, SampleTimings };
