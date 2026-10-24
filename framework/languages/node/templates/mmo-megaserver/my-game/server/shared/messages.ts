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
export class MoveRequest {
  constructor(
    readonly x: number,
    readonly sequence: number
  ) {}
}
export class PlayerPosition {
  constructor(
    readonly playerId: string,
    readonly x: number
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
