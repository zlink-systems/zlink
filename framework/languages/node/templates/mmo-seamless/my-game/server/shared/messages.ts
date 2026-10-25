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
    readonly delta: number,
    readonly sequence: bigint
  ) {}
}
export class Position {
  constructor(readonly x: number) {}
}
export class EnterZone {
  constructor(
    readonly position: Position,
    readonly sequence: bigint,
    readonly remainingMs: number
  ) {}
}
export class WorldState {
  constructor(
    readonly zoneId: string,
    readonly nodeId: string,
    readonly x: number,
    readonly sequence: bigint
  ) {}
}
export class ActionRejected {
  constructor(
    readonly reason: string,
    readonly retryAfterMs: number
  ) {}
}
export type PlayerLeftReason = 'left-zone' | 'disconnected';
export class PlayerAppeared {
  constructor(
    readonly playerId: string,
    readonly position: Position
  ) {}
}
export class PlayerMoved {
  constructor(
    readonly playerId: string,
    readonly position: Position,
    readonly sequence: bigint
  ) {}
}
export class PlayerLeft {
  constructor(
    readonly playerId: string,
    readonly reason: PlayerLeftReason
  ) {}
}
export class BorderPresence {
  constructor(
    readonly sourceZoneId: string,
    readonly playerId: string,
    readonly position: Position,
    readonly sequence: bigint,
    readonly kind: 'appeared' | 'moved' | 'left',
    readonly reason: PlayerLeftReason | null
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
