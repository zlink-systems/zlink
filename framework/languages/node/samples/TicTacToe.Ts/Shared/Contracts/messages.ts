const PacketNames = Object.freeze({
  authenticateReq: 'AuthenticateReq',
  authenticateRes: 'AuthenticateRes',
  authenticatePlayerReq: 'AuthenticatePlayerReq',
  authenticatePlayerRes: 'AuthenticatePlayerRes',
  createGameHttpReq: 'CreateGameHttpReq',
  createGameHttpRes: 'CreateGameHttpRes',
  joinGameMsg: 'JoinGameMsg',
  joinGameNotify: 'JoinGameNotify',
  joinGameFailedNotify: 'JoinGameFailedNotify',
  observeMilestoneReq: 'ObserveMilestoneReq',
  observeMilestoneRes: 'ObserveMilestoneRes',
  placeMarkReq: 'PlaceMarkReq',
  placeMarkRes: 'PlaceMarkRes',
  leaveGameMsg: 'LeaveGameMsg',
  deliverPlayNotificationMsg: 'DeliverPlayNotificationMsg',
  playerJoinedNotify: 'PlayerJoinedNotify',
  gameStateNotify: 'GameStateNotify',
  winMilestoneNotify: 'WinMilestoneNotify',
  playerWinMilestoneEvent: 'PlayerWinMilestoneEvent'
});

export interface AuthenticateReq {
  accessToken: string;
}

export class AuthenticateReq {
  accessToken: string;

  constructor(accessToken: string) {
    this.accessToken = accessToken;
  }
}

export interface AuthenticatePlayerReq {
  accessToken: string;
}

export class AuthenticatePlayerReq {
  accessToken: string;

  constructor(accessToken: string) {
    this.accessToken = accessToken;
  }
}

export interface AuthenticatePlayerRes {
  player: PlayerInfo;
}

export interface TicTacToeGameCreateReq {
  gameName: string;
  requiredLevel: number;
}

export class CreateGameHttpReq {
  gameName?: string;

  constructor(gameName?: string) {
    if (gameName !== undefined) {
      this.gameName = gameName;
    }
  }
}

export class TicTacToeGameCreateReq implements TicTacToeGameCreateReq {
  gameName: string;
  requiredLevel: number;

  constructor(gameName: string, requiredLevel: number) {
    this.gameName = gameName;
    this.requiredLevel = requiredLevel;
  }
}

export interface CreateGameHttpRes {
  roomId: string;
  gameName: string;
  playEndpoints: string[];
  playNodes: PlayNodeInfo[];
  requiredLevel: number;
}

export interface AuthenticateRes {
  player: PlayerInfo;
}

export interface PlayerInfo {
  actorId: string;
  displayName: string;
  level: number;
  wins: number;
}

export class PlayerActorCreateReq {
  constructor(readonly player: PlayerInfo) {}
}

export interface PlayNodeInfo {
  streamEndpoint: string;
}

export class JoinGameMsg {
  constructor(readonly roomId: string) {}
}

export class JoinGameNotify {
  constructor(readonly state: GameState) {}
}

export class JoinGameFailedNotify {
  constructor(readonly roomId: string, readonly error: string) {}
}

export interface TicTacToeGameJoinReq {
  roomId: string;
  player: PlayerInfo;
}

export interface TicTacToeGameJoinRes {
  state: GameState;
}

export class ObserveMilestoneReq {
}

export interface ObserveMilestoneRes {
  subscribed: boolean;
}

export class PlaceMarkReq {
  constructor(readonly cell: number) {}
}

export interface PlaceMarkRes {
  state: GameState;
}

export class LeaveGameMsg {
  constructor(readonly roomId: string) {}
}

export class PlayerJoinedNotify {
  constructor(
    readonly roomId: string,
    readonly actorId: string,
    readonly displayName: string,
    readonly level: number,
    readonly mark: string,
    readonly state: GameState
  ) {}
}

export enum GameStatus {
  WaitingForPlayers = 'WaitingForPlayers',
  InProgress = 'InProgress',
  Won = 'Won',
  Draw = 'Draw',
  TurnTimedOut = 'TurnTimedOut'
}

export const GameMarks = Object.freeze({
  x: 'X',
  o: 'O'
});

export interface GameState {
  roomId: string;
  board: string;
  status: GameStatus;
  winner: string | null;
  nextTurn: string;
  xActorId: string | null;
  oActorId: string | null;
  lastMoveActorId: string | null;
  lastMoveCell: number | null;
}

export class GameStateNotify {
  constructor(readonly state: GameState) {}
}

export class WinMilestoneNotify {
  constructor(
    readonly roomId: string,
    readonly actorId: string,
    readonly displayName: string,
    readonly wins: number
  ) {}
}

export class PlayerWinMilestoneEvent {
  constructor(
    readonly roomId: string,
    readonly actorId: string,
    readonly displayName: string,
    readonly wins: number
  ) {}
}

export interface TicTacToeActor {
  actorId: string;
  displayName: string;
  level: number;
  wins: number;
  roomId?: string;
  push(
    payload:
      | JoinGameNotify
      | JoinGameFailedNotify
      | PlayerJoinedNotify
      | GameStateNotify
      | WinMilestoneNotify
  ): Promise<void>;
}

function actorDisplayName(actorId: string): string {
  if (actorId === 'player-x') {
    return 'Player X';
  }
  if (actorId === 'player-o') {
    return 'Player O';
  }
  return 'Observer';
}

function playerInfo(accessToken: string): PlayerInfo {
  return {
    actorId: accessToken,
    displayName: actorDisplayName(accessToken),
    level: 3,
    wins: accessToken === 'player-x' ? 99 : 0
  };
}

function authenticateReq(accessToken: string): AuthenticateReq {
  return new AuthenticateReq(accessToken);
}

function authenticatePlayerReq(accessToken: string): AuthenticatePlayerReq {
  return new AuthenticatePlayerReq(accessToken);
}

function authenticatePlayerRes(accessToken: string): AuthenticatePlayerRes {
  return {
    player: playerInfo(accessToken)
  };
}

function createGameHttpReq(gameName?: string): CreateGameHttpReq {
  return new CreateGameHttpReq(gameName);
}

function authenticateRes(player: PlayerInfo): AuthenticateRes {
  return { player };
}

function joinGameNotify(state: GameState): JoinGameNotify {
  return new JoinGameNotify(state);
}

function observeMilestoneRes(subscribed: boolean): ObserveMilestoneRes {
  return { subscribed };
}

function placeMarkStreamReq(cell: number): PlaceMarkReq {
  return new PlaceMarkReq(cell);
}

function placeMarkRes(state: GameState): PlaceMarkRes {
  return { state };
}

function playerJoinedNotify(
  roomId: string,
  actorId: string,
  displayName: string,
  level: number,
  mark: string,
  state: GameState
): PlayerJoinedNotify {
  return new PlayerJoinedNotify(roomId, actorId, displayName, level, mark, state);
}

function gameStateNotify(state: GameState): GameStateNotify {
  return new GameStateNotify(state);
}

function playerWinMilestoneEvent(
  roomId: string,
  actorId: string,
  displayName: string,
  wins: number
): PlayerWinMilestoneEvent {
  return new PlayerWinMilestoneEvent(roomId, actorId, displayName, wins);
}

function winMilestoneNotify(event: PlayerWinMilestoneEvent): WinMilestoneNotify {
  return new WinMilestoneNotify(
    event.roomId,
    event.actorId,
    event.displayName,
    event.wins
  );
}

export {
  PacketNames,
  actorDisplayName,
  authenticatePlayerReq,
  authenticatePlayerRes,
  authenticateReq,
  authenticateRes,
  createGameHttpReq,
  gameStateNotify,
  joinGameNotify,
  observeMilestoneRes,
  placeMarkRes,
  placeMarkStreamReq,
  playerInfo,
  playerWinMilestoneEvent,
  playerJoinedNotify,
  winMilestoneNotify
};
