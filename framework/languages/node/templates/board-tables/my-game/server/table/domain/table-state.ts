import { randomInt } from 'node:crypto';
import { Game, Reasons } from '../../shared/constants';
import {
  HandResult,
  MaxContractInteger,
  isContractInteger,
  tryAddContractIntegers,
  type SeatView,
  type TableView
} from '../../shared/messages';
interface Seat {
  playerId: string;
  chips: bigint;
  cards: number[];
  acted: boolean;
}
export interface TableSnapshot {
  seats: Seat[];
  deck: number[];
  handNumber: bigint;
  inHand: boolean;
  turnSeat: number;
  pot: bigint;
  participants: number[];
}
export class TableState {
  constructor(private readonly randomIndex: (exclusiveMax: number) => number = randomInt) {}

  snapshot: TableSnapshot = {
    seats: [],
    deck: [],
    handNumber: 0n,
    inHand: false,
    turnSeat: Game.noSeat,
    pot: 0n,
    participants: []
  };
  seatOf(playerId: string): number {
    return this.snapshot.seats.findIndex((s) => s.playerId === playerId);
  }
  // Seats the player at admission; a seated player is admitted again without a new seat.
  admit(playerId: string): boolean {
    if (this.seatOf(playerId) >= 0) return true;
    if (this.snapshot.seats.length === Game.seats) return false;
    this.snapshot.seats.push({ playerId, chips: Game.initialChips, cards: [], acted: false });
    return true;
  }
  start(playerId: string): string | undefined {
    const s = this.snapshot;
    if (this.seatOf(playerId) < 0) return Reasons.seated;
    if (s.inHand) return Reasons.playing;
    if (s.seats.length < Game.minimumPlayers) return Reasons.players;
    const handNumber = tryAddContractIntegers(s.handNumber, 1n);
    if (handNumber === undefined) return Reasons.integerOutOfRange;
    s.deck = Array.from({ length: Game.deckSize }, (_, i) => i);
    for (let i = s.deck.length - 1; i > 0; i--) {
      const j = this.randomIndex(i + 1);
      [s.deck[i], s.deck[j]] = [s.deck[j], s.deck[i]];
    }
    s.handNumber = handNumber;
    s.inHand = true;
    s.pot = 0n;
    s.participants = s.seats.map((_, i) => i);
    s.turnSeat = s.participants[0];
    for (const seat of s.seats) {
      seat.acted = false;
      seat.cards = s.deck.splice(-Game.cardsPerPlayer);
    }
    return undefined;
  }
  bet(playerId: string, handNumber: bigint, amount: bigint): string | HandResult | undefined {
    const s = this.snapshot;
    const index = this.seatOf(playerId);
    if (index < 0) return Reasons.seated;
    const seat = s.seats[index];
    if (
      !isContractInteger(s.handNumber) ||
      !isContractInteger(seat.chips) ||
      !isContractInteger(s.pot) ||
      !isContractInteger(handNumber) ||
      typeof amount !== 'bigint' ||
      amount > MaxContractInteger
    )
      return Reasons.integerOutOfRange;
    if (!s.inHand || handNumber !== s.handNumber) return Reasons.noHand;
    if (index !== s.turnSeat) return Reasons.turn;
    if (amount < 0n || amount > seat.chips) return Reasons.amount;
    const nextPot = tryAddContractIntegers(s.pot, amount);
    if (nextPot === undefined) return Reasons.integerOutOfRange;
    const next = s.participants.find((i) => i !== index && !s.seats[i].acted);
    if (next !== undefined) {
      seat.chips -= amount;
      s.pot = nextPot;
      seat.acted = true;
      s.turnSeat = next;
      return undefined;
    }
    const score = (i: number): number =>
      s.seats[i].cards.reduce((sum, card) => sum + (card % Game.ranks) + Game.lowestRank, 0);
    const highest = Math.max(...s.participants.map(score));
    const winners = s.participants.filter((i) => score(i) === highest);
    const divisor = BigInt(winners.length);
    const share = nextPot / divisor;
    const remainder = nextPot % divisor;
    const payouts: { seat: number; chips: bigint }[] = [];
    for (let order = 0; order < winners.length; order++) {
      const winner = winners[order];
      const chipsAfterBet = s.seats[winner].chips - (winner === index ? amount : 0n);
      let chips = tryAddContractIntegers(chipsAfterBet, share);
      if (chips === undefined) return Reasons.integerOutOfRange;
      if (order === 0) {
        chips = tryAddContractIntegers(chips, remainder);
        if (chips === undefined) return Reasons.integerOutOfRange;
      }
      payouts.push({ seat: winner, chips });
    }
    const hands = s.participants.map((i) => ({
      seat: i,
      playerId: s.seats[i].playerId,
      cards: [...s.seats[i].cards]
    }));

    seat.chips -= amount;
    s.pot = nextPot;
    seat.acted = true;
    for (const payout of payouts) s.seats[payout.seat].chips = payout.chips;
    s.inHand = false;
    s.turnSeat = Game.noSeat;
    s.pot = 0n;
    for (const candidate of s.seats) candidate.cards = [];
    return new HandResult(
      s.handNumber,
      nextPot,
      winners,
      hands,
      this.seatViews()
    );
  }
  seatViews(): SeatView[] {
    return this.snapshot.seats.map((s, seat) => ({
      seat,
      playerId: s.playerId,
      chips: s.chips,
      acted: s.acted,
      cardCount: s.cards.length
    }));
  }
  view(playerId: string, tableId: string, nodeId: string): TableView {
    const s = this.snapshot;
    return {
      tableId,
      nodeId,
      yourSeat: this.seatOf(playerId),
      handNumber: s.handNumber,
      inHand: s.inHand,
      turnSeat: s.turnSeat,
      pot: s.pot,
      seats: this.seatViews()
    };
  }
  cards(playerId: string): number[] {
    return this.snapshot.inHand
      ? [...(this.snapshot.seats[this.seatOf(playerId)]?.cards ?? [])]
      : [];
  }
}
