import { randomInt } from 'node:crypto';
import { Game, Reasons } from '../../shared/constants';
import { HandResult, type SeatView, type TableView } from '../../shared/messages';
interface Seat {
  playerId: string;
  chips: number;
  cards: number[];
  acted: boolean;
}
export interface TableSnapshot {
  seats: Seat[];
  deck: number[];
  handNumber: number;
  inHand: boolean;
  turnSeat: number;
  pot: number;
  participants: number[];
}
export class TableState {
  snapshot: TableSnapshot = {
    seats: [],
    deck: [],
    handNumber: 0,
    inHand: false,
    turnSeat: Game.noSeat,
    pot: 0,
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
    s.deck = Array.from({ length: Game.deckSize }, (_, i) => i);
    for (let i = s.deck.length - 1; i > 0; i--) {
      const j = randomInt(i + 1);
      [s.deck[i], s.deck[j]] = [s.deck[j], s.deck[i]];
    }
    s.handNumber++;
    s.inHand = true;
    s.pot = 0;
    s.participants = s.seats.map((_, i) => i);
    s.turnSeat = s.participants[0];
    for (const seat of s.seats) {
      seat.acted = false;
      seat.cards = s.deck.splice(-Game.cardsPerPlayer);
    }
    return undefined;
  }
  bet(playerId: string, handNumber: number, amount: number): string | HandResult | undefined {
    const s = this.snapshot;
    const index = this.seatOf(playerId);
    if (index < 0) return Reasons.seated;
    if (!s.inHand || handNumber !== s.handNumber) return Reasons.noHand;
    if (index !== s.turnSeat) return Reasons.turn;
    const seat = s.seats[index];
    if (!Number.isSafeInteger(amount) || amount < 0 || amount > seat.chips) return Reasons.amount;
    seat.chips -= amount;
    s.pot += amount;
    seat.acted = true;
    const next = s.participants.find((i) => !s.seats[i].acted);
    if (next !== undefined) {
      s.turnSeat = next;
      return undefined;
    }
    const score = (i: number): number =>
      s.seats[i].cards.reduce((sum, card) => sum + (card % Game.ranks) + Game.lowestRank, 0);
    const highest = Math.max(...s.participants.map(score));
    const winners = s.participants.filter((i) => score(i) === highest);
    const pot = s.pot;
    const share = Math.floor(pot / winners.length);
    for (const winner of winners) s.seats[winner].chips += share;
    s.seats[winners[0]].chips += pot % winners.length;
    s.inHand = false;
    s.turnSeat = Game.noSeat;
    s.pot = 0;
    return new HandResult(
      s.handNumber,
      pot,
      winners,
      s.participants.map((i) => ({
        seat: i,
        playerId: s.seats[i].playerId,
        cards: [...s.seats[i].cards]
      })),
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
