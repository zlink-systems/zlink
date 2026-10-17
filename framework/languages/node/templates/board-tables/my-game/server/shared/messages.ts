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
    readonly handNumber: number,
    readonly amount: number
  ) {}
}
export interface SeatView {
  seat: number;
  playerId: string;
  chips: number;
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
    readonly handNumber: number,
    readonly inHand: boolean,
    readonly turnSeat: number,
    readonly pot: number,
    readonly seats: SeatView[]
  ) {}
}
export class HandDealt {
  constructor(
    readonly handNumber: number,
    readonly cards: number[]
  ) {}
}
export class HandResult {
  constructor(
    readonly handNumber: number,
    readonly pot: number,
    readonly winners: number[],
    readonly hands: RevealedHand[],
    readonly seats: SeatView[]
  ) {}
}
export class ActionRejected {
  constructor(readonly reason: string) {}
}
