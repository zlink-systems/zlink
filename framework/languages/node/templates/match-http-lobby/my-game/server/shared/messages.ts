import { Names } from './constants';
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
export class MatchRequest {
  constructor(
    readonly playerId: string,
    readonly mode: string = Names.defaultMode
  ) {}
}
export class MatchResponse {
  constructor(
    readonly roomId: string,
    readonly sessionEndpoint: string
  ) {}
}
export class JoinRoomRequest {
  constructor(
    readonly playerId: string,
    readonly roomId: string
  ) {}
}
export class MoveRequest {
  constructor(
    readonly x: number,
    readonly y: number,
    readonly sequence: number
  ) {}
}
export class ShootRequest {
  constructor(
    readonly targetId: string,
    readonly sequence: number
  ) {}
}
export class RoomViewRequest {}
export class PlayerView {
  constructor(
    readonly playerId: string,
    readonly x: number,
    readonly y: number,
    readonly hp: number
  ) {}
}
export class RoomView {
  constructor(
    readonly roomId: string = '',
    readonly nodeId: string = '',
    readonly players: PlayerView[] = []
  ) {}
}
export class ShotRejected {
  constructor(readonly reason: string) {}
}
export class ActionRejected {
  constructor(readonly reason: string) {}
}
export class MatchEnded {
  constructor(readonly winner: string = '') {}
}
