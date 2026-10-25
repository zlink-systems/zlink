import { Game, Names } from './constants';
const int32Brand: unique symbol = Symbol('Int32');
const signedInt32Min = -2_147_483_648;
const signedInt32Max = 2_147_483_647;

export type Int32 = number & { readonly [int32Brand]: true };

export function signedInt32(value: number): Int32 {
  if (!Number.isInteger(value) || value < signedInt32Min || value > signedInt32Max)
    throw new RangeError('Value is outside the signed 32-bit integer range');
  return value as Int32;
}

export function toRetryAfterMs(value: number): Int32 {
  if (Number.isNaN(value)) throw new RangeError('RetryAfterMs cannot be NaN');
  return signedInt32(Math.ceil(Math.max(0, Math.min(signedInt32Max, value))));
}

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
    readonly dx: number,
    readonly dy: number,
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
    readonly hp: number,
    readonly lastSequence: number
  ) {}
}
export class RoomView {
  constructor(
    readonly roomId: string = '',
    readonly nodeId: string = '',
    readonly phase: string = Game.playingPhase,
    readonly winner: string | null = null,
    readonly players: PlayerView[] = []
  ) {}
}
export class ActionRejected {
  readonly retryAfterMs: Int32;

  constructor(
    readonly reason: string,
    retryAfterMs: Int32 = signedInt32(0)
  ) {
    this.retryAfterMs = signedInt32(retryAfterMs);
  }
}
export class MatchEnded {
  constructor(readonly winner: string = '') {}
}
