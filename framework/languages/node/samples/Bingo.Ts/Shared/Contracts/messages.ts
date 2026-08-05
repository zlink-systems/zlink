import {
  AuthenticatePlayerReq,
  AuthenticatePlayerRes,
  AuthenticateReq,
  AuthenticateRes,
  BingoGameEndedNotify,
  BingoGameStartedNotify,
  BingoNumberDrawnNotify,
  BingoPlayerState,
  BingoRewardAcquiredEvent,
  BingoRewardAnnouncedNotify,
  BingoRoomJoinReq,
  BingoRoomJoinRes,
  BingoRoomSettingsPayload,
  BingoRoomState,
  EnsurePlayerActorReq,
  EnsurePlayerActorRes,
  GetPlayerRecordReq,
  GetPlayerRecordRes,
  MatchBingoApiReq,
  MatchBingoApiRes,
  MatchBingoReq,
  MatchBingoRes,
  ObserveBingoEventsReq,
  ObserveBingoEventsRes,
  PlayerActorTransferState,
  PlayerJoinedNotify,
  ReportBingoResultReq,
  ReportBingoResultRes,
  ReserveBingoRoomReq,
  ReserveBingoRoomRes,
  StopObservingBingoEventsReq,
  StopObservingBingoEventsRes,
  SubmitBingoCardReq,
  SubmitBingoCardRes
} from './bingo-messages.generated';

const PacketNames = Object.freeze({
  authenticateReq: 'AuthenticateReq',
  authenticatePlayerReq: 'AuthenticatePlayerReq',
  ensurePlayerActorReq: 'EnsurePlayerActorReq',
  getPlayerRecordReq: 'GetPlayerRecordReq',
  reportBingoResultReq: 'ReportBingoResultReq',
  matchBingoReq: 'MatchBingoReq',
  matchBingoApiReq: 'MatchBingoApiReq',
  bingoRoomJoinReq: 'BingoRoomJoinReq',
  submitBingoCardReq: 'SubmitBingoCardReq',
  observeBingoEventsReq: 'ObserveBingoEventsReq',
  stopObservingBingoEventsReq: 'StopObservingBingoEventsReq',
  bingoRewardAcquiredEvent: 'BingoRewardAcquiredEvent',
  playerJoinedNotify: 'PlayerJoinedNotify',
  gameStartedNotify: 'BingoGameStartedNotify',
  numberDrawnNotify: 'BingoNumberDrawnNotify',
  gameEndedNotify: 'BingoGameEndedNotify',
  rewardAnnouncedNotify: 'BingoRewardAnnouncedNotify'
});

const BingoModes = Object.freeze({ twoPlayer: 'two-player' });
const BingoSamplePlayers = Object.freeze({
  player1: 'player-1',
  player2: 'player-2',
  observer: 'observer',
  drainProbe: 'drain-probe'
});
const BingoRewardItems = Object.freeze({
  goldenDauberId: 'golden-dauber',
  goldenDauberName: 'Golden Dauber',
  legendaryRarity: 'Legendary'
});

export type BingoMode = typeof BingoModes.twoPlayer;
export enum BingoRoomStatus {
  WaitingForPlayers = 'WaitingForPlayers',
  Running = 'Running',
  Finished = 'Finished'
}

export type NumberDrawnNotify = BingoNumberDrawnNotify;
export type StateEnvelope = BingoGameStartedNotify | BingoGameEndedNotify;

function actorDisplayName(actorId: string): string { return actorId.replace('player-', 'Player '); }
function deterministicCard(actorId: string): number[] {
  return {
    [BingoSamplePlayers.player1]: [1, 2, 3, 4, 0, 6, 7, 8, 9],
    [BingoSamplePlayers.player2]: [10, 11, 12, 13, 0, 14, 15, 1, 2]
  }[actorId] ?? [1, 3, 5, 7, 0, 9, 11, 13, 15];
}

export {
  AuthenticatePlayerReq,
  AuthenticatePlayerRes,
  AuthenticateReq,
  AuthenticateRes,
  BingoModes,
  BingoGameEndedNotify,
  BingoGameStartedNotify,
  BingoNumberDrawnNotify,
  BingoPlayerState,
  BingoRewardItems,
  BingoRewardAcquiredEvent,
  BingoRewardAnnouncedNotify,
  BingoRoomJoinReq,
  BingoRoomJoinRes,
  BingoRoomSettingsPayload,
  BingoRoomState,
  BingoSamplePlayers,
  EnsurePlayerActorReq,
  EnsurePlayerActorRes,
  GetPlayerRecordReq,
  GetPlayerRecordRes,
  MatchBingoApiReq,
  MatchBingoApiRes,
  MatchBingoReq,
  MatchBingoRes,
  ObserveBingoEventsReq,
  ObserveBingoEventsRes,
  PacketNames,
  PlayerActorTransferState,
  PlayerJoinedNotify,
  ReportBingoResultReq,
  ReportBingoResultRes,
  ReserveBingoRoomReq,
  ReserveBingoRoomRes,
  StopObservingBingoEventsReq,
  StopObservingBingoEventsRes,
  SubmitBingoCardReq,
  SubmitBingoCardRes,
  actorDisplayName,
  deterministicCard,
};
