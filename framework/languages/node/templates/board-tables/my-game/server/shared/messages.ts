export const MaxContractInteger = 9_007_199_254_740_991n;

export type Int32 = number;

export function isContractInteger(value: bigint): boolean {
  return typeof value === 'bigint' && value >= 0n && value <= MaxContractInteger;
}

export function tryAddContractIntegers(left: bigint, right: bigint): bigint | undefined {
  if (!isContractInteger(left) || !isContractInteger(right) || right > MaxContractInteger - left)
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

export class FindMatch {
  constructor(readonly playerId: string) {}
}
export class MatchQueued {
  constructor(readonly ticketId: string) {}
}
export class CancelMatch {
  constructor(readonly ticketId: string) {}
}
export class MatchCancelled {
  constructor(
    readonly ticketId: string,
    readonly reason: string
  ) {}
}
export class MatchFound {
  constructor(
    readonly ticketId: string,
    readonly roomId: string,
    readonly seat: Int32
  ) {}
}
export class MatchRequest {
  constructor(
    readonly requestKey: string,
    readonly playerId: string
  ) {}
}
export class CancelTicket {
  constructor(readonly ticketId: string) {}
}
export class MatchPresence {
  constructor(
    readonly ticketId: string,
    readonly connected: boolean
  ) {}
}
export class PrepareRoom {
  constructor(
    readonly assignmentId: string,
    readonly members: string[]
  ) {}
}
export class RoomPrepared {
  constructor(
    readonly assignmentId: string,
    readonly ok: boolean
  ) {}
}
export class MatchAssigned {
  constructor(
    readonly ticketId: string,
    readonly assignmentId: string,
    readonly roomId: string
  ) {}
}
export class RoomAdmission {
  constructor(
    readonly assignmentId: string,
    readonly chips: bigint
  ) {}
}
export class TableAdmission {
  constructor(
    readonly join: JoinTableRequest,
    readonly chips: bigint
  ) {}
}
export class LoadPlayer {
  constructor(readonly playerId: string) {}
}
export class PlayerRecord {
  constructor(
    readonly playerId: string,
    readonly chips: bigint
  ) {}
}
export class CommitRound {
  constructor(
    readonly tableId: string,
    readonly handNumber: bigint,
    readonly results: readonly { playerId: string; chips: bigint }[]
  ) {}
}
export class RoundCommitted {
  constructor(
    readonly tableId: string,
    readonly handNumber: bigint
  ) {}
}
export class EnterReport {
  constructor(
    readonly ticketId: string,
    readonly assignmentId: string,
    readonly ok: boolean
  ) {}
}
export class CommitRoom {
  constructor(readonly assignmentId: string) {}
}
export class MatchCommitted {
  constructor(
    readonly assignmentId: string,
    readonly seat: Int32
  ) {}
}
export class WithdrawRoom {
  constructor(readonly assignmentId: string) {}
}
export class RoomWithdrawn {
  constructor(readonly assignmentId: string) {}
}
export class MatchReleased {
  constructor(
    readonly ticketId: string,
    readonly assignmentId: string,
    readonly requeue: boolean
  ) {}
}
export class ReturnReport {
  constructor(
    readonly ticketId: string,
    readonly assignmentId: string
  ) {}
}
