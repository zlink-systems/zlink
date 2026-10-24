export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
export class JoinWorldRequest {
  constructor(
    readonly playerId: string,
    readonly x: number
  ) {}
}
export class MoveRequest {
  constructor(
    readonly x: number,
    readonly sequence: number
  ) {}
}
export class EnterZone {
  constructor(
    readonly x: number,
    readonly sequence: number
  ) {}
}
export class WorldState {
  constructor(
    readonly zoneId: string,
    readonly nodeId: string,
    readonly x: number,
    readonly sequence: number
  ) {}
}
export class BorderPresence {
  constructor(
    readonly sourceZoneId: string,
    readonly actorId: string,
    readonly x: number,
    readonly sequence: number
  ) {}
}
export class BorderVisible {
  constructor(
    readonly observerZoneId: string = '',
    readonly sourceZoneId: string = '',
    readonly actorId: string = '',
    readonly x: number = 0,
    readonly sequence: number = 0
  ) {}
}
export class OpenDungeonRequest {
  constructor(readonly dungeonId: string) {}
}
export class DungeonOpened {
  constructor(
    readonly dungeonId: string,
    readonly nodeId: string
  ) {}
}
