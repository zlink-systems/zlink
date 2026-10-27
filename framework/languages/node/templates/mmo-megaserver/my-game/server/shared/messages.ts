export class EnterRegionRequest {
  constructor(
    readonly playerId: string,
    readonly region: string
  ) {}
}
export class JoinRegion {
  constructor(
    readonly region: string,
    readonly index: number
  ) {}
}
export class RegionFull {
  constructor(
    readonly reason: string,
    readonly region: string,
    readonly index: number
  ) {}
}
export class RegionUnavailable {
  constructor(readonly region: string, readonly reason: string) {}
}
export class MoveRequest {
  constructor(
    readonly delta: number,
    readonly sequence: bigint
  ) {}
}
export class ActionRejected {
  constructor(readonly reason: string, readonly retryAfterMs: number = 0) {}
}
export class PlayerPosition {
  constructor(
    readonly playerId: string,
    readonly x: number,
    readonly sequence: bigint
  ) {}
}
export class RegionView {
  constructor(
    readonly regionId: string = '',
    readonly nodeId: string = '',
    readonly players: PlayerPosition[] = []
  ) {}
}
export class ViewRequest {}
export class OpenDungeonRequest {
  constructor(readonly dungeonId: string) {}
}
export class DungeonInfoRequest {}
export class DungeonOpened {
  constructor(
    readonly dungeonId: string = '',
    readonly nodeId: string = ''
  ) {}
}
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
