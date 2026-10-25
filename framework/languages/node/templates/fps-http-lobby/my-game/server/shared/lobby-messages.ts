import { Names } from './constants';

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

export class PrepareRoom {
  constructor(
    readonly matchId: string,
    readonly members: string[]
  ) {}
}

export class RoomPrepared {
  constructor(readonly matchId: string) {}
}

export class HttpNodeReady {
  constructor(
    readonly id: string,
    readonly meshEndpoint: string,
    readonly streamEndpoint: string | null,
    readonly httpEndpoint?: string
  ) {}
}
