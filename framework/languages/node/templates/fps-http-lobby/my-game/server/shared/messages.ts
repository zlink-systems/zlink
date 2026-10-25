import { MatchPhase, Names } from './constants';
import { Vec3 } from './geometry';

export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
export class QueueRequest {
  constructor(
    readonly playerId: string,
    readonly mode: string = Names.defaultMode
  ) {}
}
export class RoomAdmission {
  constructor(
    readonly playerId: string,
    readonly roomId: string
  ) {}
}
export class RoomAssignment {
  constructor(readonly roomId: string) {}
}
export class MatchFound {
  constructor(readonly roomId: string = '') {}
}
export class MoveRequest {
  constructor(
    readonly delta: Vec3,
    readonly sequence: number
  ) {}
}
export class MoveResult {
  constructor(
    readonly position: Vec3,
    readonly sequence: number
  ) {}
}
export class ShootRequest {
  constructor(
    readonly aimDirection: Vec3,
    readonly sequence: number
  ) {}
}
export class RoomViewRequest {}
export class PlayerView {
  constructor(
    readonly playerId: string,
    readonly position: Vec3,
    readonly hp: number,
    readonly lastSequence: number
  ) {}
}
export class RoomView {
  constructor(
    readonly roomId: string = '',
    readonly nodeId: string = '',
    readonly players: PlayerView[] = [],
    readonly phase: MatchPhase = MatchPhase.Waiting,
    readonly winner: string | null = null
  ) {}
}
export class ShotResult {
  constructor(
    readonly outcome: string,
    readonly targetId: string | null,
    readonly sequence: number
  ) {}
}
export class ShotRejected {
  constructor(
    readonly reason: string,
    readonly retryAfterMs: number = 0
  ) {}
}
export class ActionRejected {
  constructor(
    readonly reason: string,
    readonly retryAfterMs: number = 0
  ) {}
}
export class MatchEnded {
  constructor(readonly winner: string = '') {}
}
