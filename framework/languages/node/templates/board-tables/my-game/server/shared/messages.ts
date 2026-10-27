export const MaxContractInteger = 9_007_199_254_740_991n;

export type Int32 = number;

export function isContractInteger(value: bigint): boolean {
  return typeof value === 'bigint' && value >= 0n && value <= MaxContractInteger;
}

export function tryAddContractIntegers(left: bigint, right: bigint): bigint | undefined {
  if (
    !isContractInteger(left) ||
    !isContractInteger(right) ||
    right > MaxContractInteger - left
  )
    return undefined;
  return left + right;
}

export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
export class JoinTableRequest {
  constructor(
    readonly playerId: string,
    readonly tableId: string
  ) {}
}
export class TableViewRequest {}
export class StartHandRequest {}
export class BetRequest {
  constructor(
    readonly handNumber: bigint,
    readonly amount: bigint
  ) {}
}
export interface SeatView {
  seat: number;
  playerId: string;
  chips: bigint;
  acted: boolean;
  cardCount: number;
}
export interface RevealedHand {
  seat: number;
  playerId: string;
  cards: number[];
}
export class TableView {
  constructor(
    readonly tableId: string,
    readonly nodeId: string,
    readonly yourSeat: number,
    readonly handNumber: bigint,
    readonly inHand: boolean,
    readonly turnSeat: number,
    readonly pot: bigint,
    readonly seats: SeatView[]
  ) {}
}
export class HandDealt {
  constructor(
    readonly handNumber: bigint,
    readonly cards: number[]
  ) {}
}
export class HandResult {
  constructor(
    readonly handNumber: bigint,
    readonly pot: bigint,
    readonly winners: number[],
    readonly hands: RevealedHand[],
    readonly seats: SeatView[]
  ) {}
}
export class ActionRejected {
  constructor(
    readonly reason: string,
    readonly retryAfterMs: Int32
  ) {}
}
