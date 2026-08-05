import { BingoSamplePlayers, PacketNames } from '../../Shared/Contracts/messages';
const SampleNames = {
  apiChannel: 'bingo.api',
  playMeshName: 'bingo.play',
  matchmakingMeshName: 'bingo.matchmaking',
  matchmakerSpotType: 'bingo.matchmaker',
  roomRouteChannel: 'bingo.room.route',
  roomRewardChannel: 'bingo.room.reward.publisher',
  roomRewardTopic: 'bingo.room.reward',
  playerActorType: 'bingo.player',
  roomSpotType: 'bingo.room',
  roomSpotNode: 'bingo.play',
  roomLocationPeerMonitor: 'bingo.room.location-peer',
  sessionStream: 'bingo.session.stream',
  sessionSpotNode: 'bingo.session',
  actorIds: [
    BingoSamplePlayers.player1,
    BingoSamplePlayers.player2,
    BingoSamplePlayers.observer,
    BingoSamplePlayers.drainProbe
  ],
  playerJoinedPacket: PacketNames.playerJoinedNotify,
  gameStartedPacket: PacketNames.gameStartedNotify,
  numberDrawnPacket: PacketNames.numberDrawnNotify,
  gameEndedPacket: PacketNames.gameEndedNotify
};

const SampleTimings = {
  requestTimeout: 3000
};

export { SampleNames, SampleTimings };
